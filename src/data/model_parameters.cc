//------------------------------------------------------------------------------
// Copyright (c) 2016 by contributors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//------------------------------------------------------------------------------

/*
Author: Chao Ma (mctt90@gmail.com)

This file is the implementation of the Model class.
*/

#include "src/data/model_parameters.h"

#include <pmmintrin.h>  // for SSE

#include "src/base/file_util.h"
#include "src/base/math.h"

namespace xLearn {

//------------------------------------------------------------------------------
// The Model class
//------------------------------------------------------------------------------

// Basic contributor.
void Model::Initialize(const std::string& score_func,
                  const std::string& loss_func,
                  index_t num_feature,
                  index_t num_field,
                  index_t num_K) {
  CHECK(!score_func.empty());
  CHECK(!loss_func.empty());
  CHECK_GT(num_feature, 0);
  CHECK_GE(num_field, 0);
  CHECK_GE(num_K, 0);
  score_func_ = score_func;
  loss_func_ = loss_func;
  num_feat_ = num_feature;
  num_field_ = num_field;
  num_K_ = num_K;
  // Calculate the number of model parameters
  if (score_func == "linear") {
    param_num_w_ = num_feature * 2;
  } else if (score_func == "fm") {
    param_num_w_ = num_feature * num_K * 2;
  } else if (score_func == "ffm") {
    param_num_w_ = num_feature *
                   get_aligned_k() *
                   num_field * 2;
  } else {
    LOG(FATAL) << "Unknow score function: " << score_func;
  }
  this->Initialize_w(true);
}

// To get the best performance for SSE, we need to
// allocate memory for the model parameters in aligned way
// For SSE, the align number should be 16
void Model::Initialize_w(bool set_value) {
  try {
    // Conventional malloc
    if (score_func_.compare("linear") == 0 ||
        score_func_.compare("fm") == 0) {
      param_w_ = (real_t*)malloc(param_num_w_ * sizeof(real_t));
    } else if (score_func_.compare("ffm") == 0) {
#ifdef _WIN32
      param_w_ = _aligned_malloc(
          param_num_w_ * sizeof(real_t),
          kAlignByte);
#else
      posix_memalign(
          (void**)&param_w_,
          kAlignByte,
          param_num_w_ * sizeof(real_t));
#endif
    } else {
      LOG(FATAL) << "Unknow score function: " << score_func_;
    }
  } catch (std::bad_alloc&) {
    LOG(FATAL) << "Cannot allocate enough memory for current  \
                   model parameters. Parameter size: [w] "
               << param_num_w_;
  }
  // Use distribution to transform the random unsigned
  // int generated by gen into a float in (0.0, 1.0)
  std::default_random_engine generator;
  std::uniform_real_distribution<real_t> dis(0.0, 1.0);
  // Initialize model parameters and gradient cache
  if (set_value) {
    if (score_func_.compare("linear") == 0) {
      for (index_t i = 0; i < param_num_w_; i += 2) {
        param_w_[i] = 0;
        param_w_[i+1] = 1.0;
      }
    } else if (score_func_.compare("fm") == 0) {
      for (index_t i = 0; i < param_num_w_; i += 2) {
        real_t coef = 1.0f / sqrt(num_K_);
        param_w_[i] = coef * dis(generator);
        param_w_[i+1] = 1.0;
      }
    } else if (score_func_.compare("ffm") == 0) {
      index_t k_aligned = get_aligned_k();
      real_t* w = param_w_;
      real_t coef = 1.0f / sqrt(num_K_);
      for (index_t j = 0; j < num_feat_; ++j) {
        for (index_t f = 0; f < num_field_; ++f) {
          for (index_t d = 0; d < k_aligned; ) {
            for (index_t s = 0; s < kAlign; s++, w++, d++) {
              w[0] = (d < num_K_) ? coef * dis(generator) : 0.0;
              w[kAlign] = 1.0;
            }
            w += kAlign;
          }
        }
      }
    } else {
      LOG(FATAL) << "Unknow score function: " << score_func_;
    }
  }
}

// Initialize model from a checkpoint file
Model::Model(const std::string& filename) {
  CHECK_NE(filename.empty(), true);
  if (this->Deserialize(filename) == false) {
    printf("Cannot Load model from the file: %s\n",
           filename.c_str());
    exit(0);
  }
}

// Serialize current model to a disk file
void Model::Serialize(const std::string& filename) {
  CHECK_NE(filename.empty(), true);
  FILE* file = OpenFileOrDie(filename.c_str(), "w");
  // Write score function
  WriteStringToFile(file, score_func_);
  // Write loss function
  WriteStringToFile(file, loss_func_);
  // Write feature num
  WriteDataToDisk(file, (char*)&num_feat_, sizeof(num_feat_));
  // Write field num
  WriteDataToDisk(file, (char*)&num_field_, sizeof(num_field_));
  // Write K
  WriteDataToDisk(file, (char*)&num_K_, sizeof(num_K_));
  // Write w
  this->serialize_w(file);
  Close(file);
}

// Deserialize model from a checkpoint file
bool Model::Deserialize(const std::string& filename) {
  CHECK_NE(filename.empty(), true);
  FILE* file = OpenFileOrDie(filename.c_str(), "r");
  if (file == NULL) { return false; }
  // Read score function
  ReadStringFromFile(file, score_func_);
  // Read loss function
  ReadStringFromFile(file, loss_func_);
  // Read feature num
  ReadDataFromDisk(file, (char*)&num_feat_, sizeof(num_feat_));
  // Read field num
  ReadDataFromDisk(file, (char*)&num_field_, sizeof(num_field_));
  // Read K
  ReadDataFromDisk(file, (char*)&num_K_, sizeof(num_K_));
  // Read w
  this->deserialize_w(file);
  Close(file);
  return true;
}

// Serialize w
void Model::serialize_w(FILE* file) {
  // Write size of w
  WriteDataToDisk(file, (char*)&param_num_w_, sizeof(param_num_w_));
  // Write w
  WriteDataToDisk(file, (char*)param_w_, sizeof(real_t)*param_num_w_);
}

// Deserialize w
void Model::deserialize_w(FILE* file) {
  // Read size of w
  ReadDataFromDisk(file, (char*)&param_num_w_, sizeof(param_num_w_));
  // Allocate memory
  this->Initialize_w(false);  /* do not set value here */
  // Read data
  ReadDataFromDisk(file, (char*)param_w_, sizeof(real_t)*param_num_w_);
}

}  // namespace xLearn
