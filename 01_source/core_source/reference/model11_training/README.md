# 模型11训练与数据整理

这里保留模型11最终训练阶段使用的入口和相关数据处理代码。它与早期同名m11_pair实验是两套记录；本作品采用rgb_rebuild中的standard_visiodect_adapt_11结果。

|文件|用途|
|---|---|
|prepare.py|整理基础数据、标签与模型初始化配置|
|catalog.py|记录原始来源目录与数据入口|
|prepare_multisource_plan.py|生成多来源训练阶段的样本与参数计划|
|train.py|读取阶段计划，调用Ultralytics训练，保存进度和权重|
|common.py|路径、JSON读写、文件摘要和时间函数|
|data_role_guard.py|核对训练与开发角色、来源组和样本记录|
|prepare_multisource_data.py|按来源清单生成多来源训练与开发输入|
|prepare_source_adaptation.py|补入已核对的公开视频帧及其标签|
|prepare_train_coverage_experiment.py|组织来源平衡阶段的输入与参数|
|prepare_source_balance_plan.py|生成来源平衡的训练计划|
|prepare_visiodect_pilot_v1.py|选取并取得带图像与标签配对的VisioDECT样本|
|admit_visiodect_clear_v1.py|按人工全图检查结果接纳配对样本|
|prepare_visiodect_expanded_v1.py|准备后续同来源训练输入|
|admit_visiodect_clear_expanded_v1.py|接纳补充的清晰标注样本并生成适配清单|
|evaluate_dev.py|按固定开发图像统计检测结果|
|scoring.py|按IoU匹配检测与标注并计算P、R和F1|
|training_parameters.json|模型11最终阶段的实际训练参数|

图像处理与样本提取另用NumPy、OpenCV和requests。原训练环境为PyTorch 2.11.0+cu128、Ultralytics 8.4.162。最终阶段从standard_train_coverage_balance_10继续训练4轮，训练去重图像6932张、每轮曝光12160次，开发图像1629张。

原脚本中的W指向当时工作目录，计划、清单和中间权重保存在D:/FPGA_Gaming/pc_runs/rgb_rebuild_20261022_v1；脚本仍保留这些路径和阶段时限，便于对照原实验。新环境复现时需要按来源重新准备数据、标签和计划，再调用相同训练接口。数据来源及采用数量见材料根03_model/DATASET_SOURCES.md。当前成品直接使用随附模型11和INT8部署文件。
