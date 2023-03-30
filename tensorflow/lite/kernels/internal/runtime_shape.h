/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#ifndef TENSORFLOW_LITE_KERNELS_INTERNAL_RUNTIME_SHAPE_H_
#define TENSORFLOW_LITE_KERNELS_INTERNAL_RUNTIME_SHAPE_H_

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>

#include "tensorflow/lite/kernels/internal/compatibility.h"

namespace tflite {

template <int N>
struct Dims {
  int sizes[N];
  int strides[N];
};

class RuntimeShape {
 public:
  // Shapes with dimensions up to 5 are stored directly in the structure, while
  // larger shapes are separately allocated.
  static constexpr int kMaxSmallSize = 5;

  RuntimeShape& operator=(RuntimeShape const&) = delete;

  RuntimeShape() : size_(0) {}

  explicit RuntimeShape(int dimensions_count) : size_(dimensions_count) {
    if (dimensions_count > kMaxSmallSize) {
      dims_pointer_ = new int32_t[dimensions_count];
    }
  }

  RuntimeShape(int shape_size, int32_t value) : size_(0) {
    Resize(shape_size);
    for (int i = 0; i < shape_size; ++i) {
      SetDim(i, value);
    }
  }

  RuntimeShape(int dimensions_count, const int32_t* dims_data) : size_(0) {
    ReplaceWith(dimensions_count, dims_data);
  }

  RuntimeShape(const std::initializer_list<int> init_list) : size_(0) {
    BuildFrom(init_list);
  }

  // Avoid using this constructor.  We should be able to delete it when C++17
  // rolls out.
  RuntimeShape(RuntimeShape const& other) : size_(other.DimensionsCount()) {
    if (size_ > kMaxSmallSize) {
      dims_pointer_ = new int32_t[size_];
    }
    std::memcpy(DimsData(), other.DimsData(), sizeof(int32_t) * size_);
  }

  bool operator==(const RuntimeShape& comp) const {
    return this->size_ == comp.size_ &&
           std::memcmp(DimsData(), comp.DimsData(), size_ * sizeof(int32_t)) ==
               0;
  }

  ~RuntimeShape() {
    if (size_ > kMaxSmallSize) {
      delete[] dims_pointer_;
    }
  }

  inline int32_t DimensionsCount() const { return size_; }
  inline int32_t Dims(int i) const {
    TFLITE_DCHECK_GE(i, 0);
    TFLITE_DCHECK_LT(i, size_);
    return size_ > kMaxSmallSize ? dims_pointer_[i] : dims_[i];
  }
  inline void SetDim(int i, int32_t val) {
    TFLITE_DCHECK_GE(i, 0);
    TFLITE_DCHECK_LT(i, size_);
    if (size_ > kMaxSmallSize) {
      dims_pointer_[i] = val;
    } else {
      dims_[i] = val;
    }
  }

  inline int32_t* DimsData() {
    return size_ > kMaxSmallSize ? dims_pointer_ : dims_;
  }
  inline const int32_t* DimsData() const {
    return size_ > kMaxSmallSize ? dims_pointer_ : dims_;
  }
  // The caller must ensure that the shape is no bigger than 5-D.
  inline const int32_t* DimsDataUpTo5D() const { return dims_; }

  inline void Resize(int dimensions_count) {
    if ((size_ <= kMaxSmallSize && dimensions_count <= kMaxSmallSize) ||
        (size_ > kMaxSmallSize && dimensions_count > kMaxSmallSize &&
         dimensions_count <= size_)) {
      size_ = dimensions_count;
    } else {
      int32_t* old_dims_pointer = dims_pointer_;
      int32_t old_small_data[kMaxSmallSize];
      auto old_size = size_;
      if (old_size <= kMaxSmallSize) {
        memcpy(dims_, old_small_data, sizeof(int32_t) * old_size);
        old_dims_pointer = old_small_data;
      }
      if (dimensions_count > kMaxSmallSize) {
        dims_pointer_ = new int32_t[dimensions_count];
      }
      const size_t count =
          dimensions_count < old_size ? dimensions_count : old_size;
      memcpy(dims_pointer_, old_dims_pointer, sizeof(int32_t) * count);
      size_ = dimensions_count;
      if (old_size > kMaxSmallSize) {
        delete[] old_dims_pointer;
      }
    }
  }

  inline void ReplaceWith(int dimensions_count, const int32_t* dims_data) {
    Resize(dimensions_count);
    int32_t* dst_dims = DimsData();
    std::memcpy(dst_dims, dims_data, dimensions_count * sizeof(int32_t));
  }

  template <typename T>
  inline void BuildFrom(const T& src_iterable) {
    const int dimensions_count =
        std::distance(src_iterable.begin(), src_iterable.end());
    Resize(dimensions_count);
    int32_t* data = DimsData();
    for (auto it : src_iterable) {
      *data = it;
      ++data;
    }
  }

  // This will probably be factored out. Old code made substantial use of 4-D
  // shapes, and so this function is used to extend smaller shapes. Note that
  // (a) as Dims<4>-dependent code is eliminated, the reliance on this should be
  // reduced, and (b) some kernels are stricly 4-D, but then the shapes of their
  // inputs should already be 4-D, so this function should not be needed.
  inline static RuntimeShape ExtendedShape(int new_shape_size,
                                           const RuntimeShape& shape) {
    return RuntimeShape(new_shape_size, shape, 1);
  }

  inline void BuildFrom(const std::initializer_list<int> init_list) {
    BuildFrom<const std::initializer_list<int>>(init_list);
  }

  // Returns the total count of elements, that is the size when flattened into a
  // vector.
  inline int FlatSize() const {
    int buffer_size = 1;
    const int* dims_data = reinterpret_cast<const int*>(DimsData());
    for (int i = 0; i < size_; i++) {
      buffer_size *= dims_data[i];
    }
    return buffer_size;
  }

  bool operator!=(const RuntimeShape& comp) const { return !((*this) == comp); }

 private:
  // For use only by ExtendedShape(), written to guarantee (return-value) copy
  // elision in C++17.
  // This creates a shape padded to the desired size with the specified value.
  RuntimeShape(int new_shape_size, const RuntimeShape& shape, int pad_value)
      : size_(0) {
    // If the following check fails, it is likely because a 4D-only kernel is
    // being used with an array of larger dimension count.
    TFLITE_CHECK_GE(new_shape_size, shape.DimensionsCount());
    Resize(new_shape_size);
    const int size_increase = new_shape_size - shape.DimensionsCount();
    for (int i = 0; i < size_increase; ++i) {
      SetDim(i, pad_value);
    }
    std::memcpy(DimsData() + size_increase, shape.DimsData(),
                sizeof(int32_t) * shape.DimensionsCount());
  }

  int32_t size_;
  union {
    int32_t dims_[kMaxSmallSize];
    int32_t* dims_pointer_;
  };
};

// Converts inference-style shape to legacy tflite::Dims<4>.
inline tflite::Dims<4> ToRuntimeDims(const tflite::RuntimeShape& array_shape) {
  tflite::Dims<4> result;
  const int dimensions_count = array_shape.DimensionsCount();
  TFLITE_CHECK_LE(dimensions_count, 4);
  int cum_prod = 1;
  for (int i = 0; i < 4; i++) {
    const int new_dim =
        (i < dimensions_count) ? array_shape.Dims(dimensions_count - 1 - i) : 1;
    result.sizes[i] = new_dim;
    result.strides[i] = cum_prod;
    cum_prod *= new_dim;
  }
  return result;
}

// TODO(b/80418076): Move to legacy ops file, update invocations.
inline RuntimeShape DimsToShape(const tflite::Dims<4>& dims) {
  return RuntimeShape(
      {dims.sizes[3], dims.sizes[2], dims.sizes[1], dims.sizes[0]});
}

// Since tensors with '0' in their shape are valid in TF, these offset functions
// allow that as long as the corresponding index is also 0. It is upto the
// calling ops to ensure that they perform verification checks on tensor shapes
// if they don't support a particular behavior.

inline int Offset(const RuntimeShape& shape, int i0, int i1, int i2, int i3) {
  TFLITE_DCHECK_EQ(shape.DimensionsCount(), 4);
  const int* dims_data = reinterpret_cast<const int*>(shape.DimsDataUpTo5D());
  TFLITE_DCHECK((dims_data[0] == 0 && i0 == 0) ||
                (i0 >= 0 && i0 < dims_data[0]));
  TFLITE_DCHECK((dims_data[1] == 0 && i1 == 0) ||
                (i1 >= 0 && i1 < dims_data[1]));
  TFLITE_DCHECK((dims_data[2] == 0 && i2 == 0) ||
                (i2 >= 0 && i2 < dims_data[2]));
  TFLITE_DCHECK((dims_data[3] == 0 && i3 == 0) ||
                (i3 >= 0 && i3 < dims_data[3]));
  return ((i0 * dims_data[1] + i1) * dims_data[2] + i2) * dims_data[3] + i3;
}

inline int Offset(const RuntimeShape& shape, int i0, int i1, int i2, int i3,
                  int i4) {
  TFLITE_DCHECK_EQ(shape.DimensionsCount(), 5);
  const int* dims_data = reinterpret_cast<const int*>(shape.DimsDataUpTo5D());
  TFLITE_DCHECK((dims_data[0] == 0 && i0 == 0) ||
                (i0 >= 0 && i0 < dims_data[0]));
  TFLITE_DCHECK((dims_data[1] == 0 && i1 == 0) ||
                (i1 >= 0 && i1 < dims_data[1]));
  TFLITE_DCHECK((dims_data[2] == 0 && i2 == 0) ||
                (i2 >= 0 && i2 < dims_data[2]));
  TFLITE_DCHECK((dims_data[3] == 0 && i3 == 0) ||
                (i3 >= 0 && i3 < dims_data[3]));
  TFLITE_DCHECK((dims_data[4] == 0 && i4 == 0) ||
                (i4 >= 0 && i4 < dims_data[4]));
  return (((i0 * dims_data[1] + i1) * dims_data[2] + i2) * dims_data[3] + i3) *
             dims_data[4] +
         i4;
}

inline int Offset(const RuntimeShape& shape, int* index) {
  return Offset(shape, index[0], index[1], index[2], index[3]);
}

}  // namespace tflite

#endif  // ENSORFLOW_LITE_KERNELS_INTERNAL_RUNTIME_SHAPE_H_
