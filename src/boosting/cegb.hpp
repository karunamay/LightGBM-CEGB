#ifndef LIGHTGBM_BOOSTING_CEGB_H_
#define LIGHTGBM_BOOSTING_CEGB_H_

#include <LightGBM/boosting.h>
#include <LightGBM/tree_learner.h>

// FIXME:
#include "../treelearner/cegb_tree_learner.h"
#include "score_updater.hpp"
#include "gbdt.h"

#include <cstdio>
#include <vector>
#include <string>
#include <fstream>

namespace LightGBM {

/*!
* \brief CEGB algorithm implementation. including Training, prediction.
*/
class CEGB: public GBDT {
public:
  /*!
  * \brief Constructor
  */
  CEGB() : GBDT() { }
  /*!
  * \brief Destructor
  */
  ~CEGB() { }
  /*!
  * \brief Initialization logic
  * \param config Config for boosting
  * \param train_data Training data
  * \param objective_function Training objective function
  * \param training_metrics Training metrics
  * \param output_model_filename Filename of output model
  */
  void Init(const BoostingConfig* config, const Dataset* train_data, const ObjectiveFunction* objective_function,
            const std::vector<const Metric*>& training_metrics) override {
    GBDT::Init(config, train_data, objective_function, training_metrics);
  }

  void ResetTrainingData(const BoostingConfig* config, const Dataset* train_data, const ObjectiveFunction* objective_function,
                         const std::vector<const Metric*>& training_metrics) override {
    InitTreeLearner(config);
    GBDT::ResetTrainingData(config, train_data, objective_function, training_metrics);
    ResetFeatureTracking();
  }

  /*!
  * \brief one training iteration
  */
  bool TrainOneIter(const score_t* gradient, const score_t* hessian, bool is_eval) override {
    if (gbdt_config_->cegb_config.gm_mode)
      iter_features_used.clear();

    bool res = GBDT::TrainOneIter(gradient, hessian, is_eval);

    if (gbdt_config_->cegb_config.gm_mode) {
      for (int i_feature : iter_features_used)
        coupled_feature_used[i_feature] = true;
    }

    return res;
  }

private:
    std::vector<bool> lazy_feature_used;
    std::vector<bool> coupled_feature_used;
    std::vector<int> iter_features_used;

  void ResetFeatureTracking()
  {
    lazy_feature_used.clear();
    lazy_feature_used.resize(train_data_->num_total_features() * train_data_->num_data());
    coupled_feature_used.clear();
    coupled_feature_used.resize(train_data_->num_total_features());
  }

  void InitTreeLearner(const BoostingConfig* config)
  {
    if (config->device_type != std::string("cpu"))
      Log::Fatal("CEGB currently only supports CPU tree learner, '%s' is unsupported.", config->device_type);
    if (config->tree_learner_type != std::string("serial"))
      Log::Fatal("CEGB currently only supports serial tree learner, '%s' is unsupported.", config->tree_learner_type);
    if (tree_learner_ != nullptr)
      return;
    tree_learner_ = std::unique_ptr<TreeLearner>((TreeLearner *)new CEGBTreeLearner(&config->tree_config, &config->cegb_config, lazy_feature_used, coupled_feature_used));
  }
};

}  // namespace LightGBM
#endif   // LightGBM_BOOSTING_CEGB_H_
