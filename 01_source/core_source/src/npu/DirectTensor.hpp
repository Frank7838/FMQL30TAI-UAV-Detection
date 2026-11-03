// 直接分配UINT8 Tensor并拷贝预处理结果，保持Icraft输入布局与类型。

#pragma once
// For true UInt8 RGB only, convertTo(CV_8U) is an identical byte copy.
// Copy once into a fresh SDK-owned Host allocation. No Tensor shares mutable
// RGB storage; non-UInt8/non-fixed/non-contiguous inputs use vendor helper.
// 只对固定640×640连续RGB/UInt8走直接分配路径；不满足类型或布局时回退厂商转换。
// 返回Tensor拥有独立Host存储；图像缓冲下一次复用不会改写已提交输入。
inline icraft::xrt::Tensor DirectUInt8Tensor(cv::Mat &image, const icraft::xrt::Network &network) {
    auto dtype = network.inputs()[0].tensorType().clone();
    auto storage = dtype->element_dtype.getStorageType();
    if (image.type() != CV_8UC3 || image.rows != 640 || image.cols != 640 ||
        !image.isContinuous() || !storage.is<icraft::xir::IntegerType>() ||
        !storage.cast<icraft::xir::IntegerType>().isUInt8() || dtype->element_dtype.bits() != 8)
        return CvMat2Tensor(image, network);
    // 显式保持NHWC：[batch=1,height=640,width=640,channels=3]，不额外除255或改量化类型。
    dtype.setShape(std::vector<int64_t>{1, 640, 640, 3});
    auto tensor = icraft::xrt::Tensor(dtype).mallocOn(icraft::xrt::HostDevice::MemRegion());
    if (tensor.data().cptr() == nullptr || tensor.dtype().numElements() != 640 * 640 * 3 ||
        image.total() * image.elemSize() != 640 * 640 * 3)
        throw std::runtime_error("direct UInt8 real buffer contract");
    std::memcpy(tensor.data().cptr(), image.data, 640 * 640 * 3);
    return tensor;
}
