# 模型11数据来源



下表是模型11实际使用的子集，不是各公开数据集的总规模。训练去重图像6932张，每轮清单曝光12160次；开发验证1629张已用于调试与选模。重复采样不增加独立样本，展示片段保持已有TRAIN角色。

|来源|本项目训练图像|每轮曝光|核心DEV|原作者来源|
|---|---:|---:|---:|---|
|Anti-UAV日间RGB|3421|3421|1067|[原作者仓库](https://github.com/ucas-vg/Anti-UAV)、[论文](https://arxiv.org/abs/2101.08466)|
|Anti-UAV已有低照RGB|335|335|96|同上；可见光低照片段，与热IR分开|
|Det-Fly|137|548|0|[原作者仓库](https://github.com/Jake-WU/Det-Fly)；Zheng等，IEEE RA-L 2021，6(2):1020–1027|
|Halmstad可见光片段|373|1492|0|[原作者仓库](https://github.com/DroneDetectionThesis/Drone-detection-dataset)、[数据论文](https://doi.org/10.1016/j.dib.2021.107521)|
|DUT Anti-UAV|2468|4936|466|[原作者仓库](https://github.com/wangdongdut/DUT-Anti-UAV)、[论文](https://arxiv.org/abs/2205.10851)|
|ICG-Drone室外子集|158|948|0|[原作者仓库](https://github.com/NTU-ICG/multidrone-detection-tracking)、[论文](https://doi.org/10.1109/TIV.2024.3360433)|
|VisioDECT选定机型|40|480|0|[作者发布的版本1](https://www.kaggle.com/datasets/simeonajakwe/visiodect-for-scenario-based-multi-drone-detection/versions/1)、[版本DOI](https://doi.org/10.34740/kaggle/dsv/13155551)|
|合计|6932|12160|1629|—|

Halmstad使用已有TRAIN视频片段，ICG纳入作者包中选定的室外TRAIN成员；VisioDECT对应Simeon Ajakwe发布的版本1。来源名称表示数据家族，不表示这些图像来自互相独立的采集场地。

训练与开发数据按来源、视频组和用途分别保存，并核对文件及解码像素是否重复；INT8校准只用TRAIN。1629张DEV已用于调试和选模，因此精度用于开发与部署核验，不作为独立TEST成绩。公开片源的原始拍摄关系和预训练数据仍有追溯限制。

本仓库不提交图像、数据集视频或标注全集。原始标签与划分清单保留在本机pc_runs/rgb_rebuild_20261022_v1，下载和使用遵循各原作者的说明。视频中的NPU展示采用Halmstad家族的已有TRAIN片段，傍晚窗外实拍是本项目现场记录，两者分别说明。

## 保留IR扩展的数据

IR分支使用Anti-UAV300红外数据，来源同[Anti-UAV原作者仓库](https://github.com/ucas-vg/Anti-UAV)。其训练与开发记录独立于现用RGB模型11，不能合并精度分母。低光RC1的608张评分图像属于已曝光开发集，552个Host/NPU输出框全部匹配；不是红外相机现场夜间测试。既有权重、部署程序与原始记录保留在`deliveries/model_rc1_lowlight_IR_20261020_v1`。

该IR候选优化的TRAIN清单为1015张，含日间IR 510张、低光IR 505张；这不是整个预训练语料的规模。INT8校准从低光TRAIN中取200张（180正、20负），未使用DEV。原图640×512，上下各补64行至640×640。608张DEV与该TRAIN的声明分组、记录ID均无交集；这些检查不能代替拍摄场景独立性证明。IR的图像、权重、分母和结果与RGB分开，作为扩展材料，不作为模型11的夜间性能。
