#ifndef LIGHTGBM_FEATURE_GROUP_H_
#define LIGHTGBM_FEATURE_GROUP_H_

#include <LightGBM/utils/random.h>

#include <LightGBM/meta.h>
#include <LightGBM/bin.h>

#include <cstdio>
#include <memory>
#include <vector>

namespace LightGBM {

class Dataset;
class DatasetLoader;
/*! \brief Using to store data and providing some operations on one feature group*/
class FeatureGroup {
public:
  friend Dataset;
  friend DatasetLoader;
  /*!
  * \brief Constructor
  * \param num_feature number of features of this group
  * \param bin_mappers Bin mapper for features
  * \param num_data Total number of data
  * \param is_enable_sparse True if enable sparse feature
  */
  FeatureGroup(int num_feature,
    std::vector<std::unique_ptr<BinMapper>>& bin_mappers,
    data_size_t num_data, bool is_enable_sparse) : num_feature_(num_feature) {
    CHECK(static_cast<int>(bin_mappers.size()) == num_feature);
    // use bin at zero to store default_bin
    num_total_bin_ = 1;
    bin_offsets_.emplace_back(num_total_bin_);
    int cnt_non_zero = 0;
    for (int i = 0; i < num_feature_; ++i) {
      bin_mappers_.emplace_back(bin_mappers[i].release());
      auto num_bin = bin_mappers_[i]->num_bin();
      if (bin_mappers_[i]->GetDefaultBin() == 0) {
        num_bin -= 1;
      }
      num_total_bin_ += num_bin;
      bin_offsets_.emplace_back(num_total_bin_);
      cnt_non_zero += static_cast<int>(num_data * (1.0f - bin_mappers_[i]->sparse_rate()));
    }
    double sparse_rate = 1.0f - static_cast<double>(cnt_non_zero) / (num_data);
    bin_data_.reset(Bin::CreateBin(num_data, num_total_bin_,
      sparse_rate, is_enable_sparse, &is_sparse_));
  }
  /*!
  * \brief Constructor from memory
  * \param memory Pointer of memory
  * \param num_all_data Number of global data
  * \param local_used_indices Local used indices, empty means using all data
  */
  FeatureGroup(const void* memory, data_size_t num_all_data,
    const std::vector<data_size_t>& local_used_indices) {
    const char* memory_ptr = reinterpret_cast<const char*>(memory);
    // get is_sparse
    is_sparse_ = *(reinterpret_cast<const bool*>(memory_ptr));
    memory_ptr += sizeof(is_sparse_);
    num_feature_ = *(reinterpret_cast<const int*>(memory_ptr));
    memory_ptr += sizeof(num_feature_);
    // get bin mapper
    bin_mappers_.clear();
    bin_offsets_.clear();
    // start from 1, due to need to store zero bin in this slot
    num_total_bin_ = 1;
    bin_offsets_.emplace_back(num_total_bin_);
    for (int i = 0; i < num_feature_; ++i) {
      bin_mappers_.emplace_back(new BinMapper(memory_ptr));
      auto num_bin = bin_mappers_[i]->num_bin();
      if (bin_mappers_[i]->GetDefaultBin() == 0) {
        num_bin -= 1;
      }
      num_total_bin_ += num_bin;
      bin_offsets_.emplace_back(num_total_bin_);
      memory_ptr += bin_mappers_[i]->SizesInByte();
    }
    data_size_t num_data = num_all_data;
    if (!local_used_indices.empty()) {
      num_data = static_cast<data_size_t>(local_used_indices.size());
    }
    if (is_sparse_) {
      bin_data_.reset(Bin::CreateSparseBin(num_data, num_total_bin_));
    } else {
      bin_data_.reset(Bin::CreateDenseBin(num_data, num_total_bin_));
    }
    // get bin data
    bin_data_->LoadFromMemory(memory_ptr, local_used_indices);
  }
  /*! \brief Destructor */
  ~FeatureGroup() {
  }

  /*!
  * \brief Push one record, will auto convert to bin and push to bin data
  * \param tid Thread id
  * \param idx Index of record
  * \param value feature value of record
  */
  inline void PushData(int tid, int sub_feature_idx, data_size_t line_idx, double value) {
    uint32_t bin = bin_mappers_[sub_feature_idx]->ValueToBin(value);
    if (bin == bin_mappers_[sub_feature_idx]->GetDefaultBin()) { return; }
    bin += bin_offsets_[sub_feature_idx];
    if (bin_mappers_[sub_feature_idx]->GetDefaultBin() == 0) {
      bin -= 1;
    }
    bin_data_->Push(tid, line_idx, bin);
  }

  inline void CopySubset(const FeatureGroup* full_feature, const data_size_t* used_indices, data_size_t num_used_indices) {
    bin_data_->CopySubset(full_feature->bin_data_.get(), used_indices, num_used_indices);
  }

  inline BinIterator* SubFeatureIterator(int sub_feature) {
    uint32_t min_bin = bin_offsets_[sub_feature];
    uint32_t max_bin = bin_offsets_[sub_feature + 1] - 1;
    uint32_t default_bin = bin_mappers_[sub_feature]->GetDefaultBin();
    return bin_data_->GetIterator(min_bin, max_bin, default_bin);
  }

  inline data_size_t Split(
    int sub_feature,
    uint32_t threshold,
    data_size_t* data_indices, data_size_t num_data,
    data_size_t* lte_indices, data_size_t* gt_indices) const {

    uint32_t min_bin = bin_offsets_[sub_feature];
    uint32_t max_bin = bin_offsets_[sub_feature + 1] - 1;
    uint32_t default_bin = bin_mappers_[sub_feature]->GetDefaultBin();
    return bin_data_->Split(min_bin, max_bin, default_bin,
      threshold, data_indices, num_data, lte_indices, gt_indices);
  }
  /*!
  * \brief From bin to feature value
  * \param bin
  * \return FeatureGroup value of this bin
  */
  inline double BinToValue(int sub_feature_idx, uint32_t bin) const {
    return bin_mappers_[sub_feature_idx]->BinToValue(bin);
  }

  /*!
  * \brief Save binary data to file
  * \param file File want to write
  */
  void SaveBinaryToFile(FILE* file) const {
    fwrite(&is_sparse_, sizeof(is_sparse_), 1, file);
    fwrite(&num_feature_, sizeof(num_feature_), 1, file);
    for (int i = 0; i < num_feature_; ++i) {
      bin_mappers_[i]->SaveBinaryToFile(file);
    }
    bin_data_->SaveBinaryToFile(file);
  }
  /*!
  * \brief Get sizes in byte of this object
  */
  size_t SizesInByte() const {
    size_t ret = sizeof(is_sparse_) + sizeof(num_feature_);
    for (int i = 0; i < num_feature_; ++i) {
      ret += bin_mappers_[i]->SizesInByte();
    }
    ret += bin_data_->SizesInByte();
    return ret;
  }
  /*! \brief Disable copy */
  FeatureGroup& operator=(const FeatureGroup&) = delete;
  /*! \brief Disable copy */
  FeatureGroup(const FeatureGroup&) = delete;

private:
  /*! \brief Number of features */
  int num_feature_;
  /*! \brief Bin mapper for sub features */
  std::vector<std::unique_ptr<BinMapper>> bin_mappers_;
  /*! \brief Bin offsets for sub features */
  std::vector<uint32_t> bin_offsets_;
  /*! \brief Bin data of this feature */
  std::unique_ptr<Bin> bin_data_;
  /*! \brief True if this feature is sparse */
  bool is_sparse_;
  int num_total_bin_;
};


}  // namespace LightGBM

#endif   // LIGHTGBM_FEATURE_GROUP_H_