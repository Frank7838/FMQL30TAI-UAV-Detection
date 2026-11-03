# 模型与数据来源

model11_original_best.pt是当前固定的模型11原权重。板端INT8模型在02_runtime/board/payload/model，配置在同目录的int8.yaml。它们用于NPU运行，不需要重新导出或校准。

DATASET_SOURCES.md列出实际使用的数据集、训练与开发规模以及原作者链接。仓库不包含训练图像、数据集视频或标注全集。技术文档与演示仅保留少量结果示例，并说明其用途。
