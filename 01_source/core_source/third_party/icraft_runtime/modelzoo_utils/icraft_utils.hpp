#pragma once

// mzu includes
#include "algorithm_utils.hpp"
#include "et_device.hpp"
#include "file_utils.hpp"
#include "demo_utils.hpp"
#include "log_utils.hpp"

// icraft includes
#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/buyi_device.h>
#include <icraft-xrt/dev/zg330_device.h>
#include <icraft-backends/hostbackend/utils.h>
#include <icraft-backends/hostbackend/backend.h>
#include <icraft-backends/buyibackend/buyibackend.h>
#include <icraft-backends/zg330backend/zg330backend.h>
#include "icraft-xir/serialize/json.h"
#include "icraft-xir/ops/align_axis.h"
#include "icraft-xir/ops/prune_axis.h"
#include "icraft-xir/ops/cast.h"
#include <icraft-backends/hostbackend/cuda/device.h>

#include <opencv2/opencv.hpp>
#include <fstream>
#include <regex>

std::map<std::string, std::string> STAGE = {
	{"p", "parsed"},
	{"o", "optimized"},
	{"q", "quantized"},
	{"a", "adapted"},
	{"g", "BY/ZG"},
};

struct FPAIConfig
{
	std::string device_url = "axi://ql100aiu?npu=0x40000000&dma=0x80000000"; // buyi设备URL
	bool speed_mode = false;												 // 是否开启极速模式
	bool compress_ftmp = false;												 // 是否压缩ftmp
	bool mmu_mode = true;													 // 是否开启MMU模式
	int ocm_option = 4;														 // OCM选项
	std::string run_backend = "buyi";										 // 运行后端，buyi或zg330
	bool enable_profile = false;											 // 是否启用性能分析
};

// 合并算子之后的计时 ，因为合并后id改了 所以要从getForwards里获取实际的算子属性
inline void calctime_detail(const std::string &runBackend, icraft::xrt::Session &session)
{
	if (runBackend == "zg330")
	{
		//--- Get network name and setup output file
		auto network_name = session->network_view.network()->name;
		checkDir("./logs/");
		std::string filePath = "./logs/" + network_name + "_time" + ".txt";
		std::ofstream ofs(filePath.c_str(), std::ios::out);

		float total_hard_time = 0;
		float total_time = 0;
		float total_memcpy_time = 0;
		float total_other_time = 0;
		float hardop_total_time = 0;
		float hardop_hard_time = 0;
		float hardop_memcpy_time = 0;
		float io_total_time = 0;
		float io_memcpytime = 0;
		float io_hardtime = 0;
		float io_othertime = 0;
		float io_process_time = 0;

		float icore_in_time = 0;
		float icore_time = 0;
		float cpu_time = 0;

		bool imk_on = false;
		bool post_on = false;
		float icore_out_time = 0;
		float customop_total_time = 0;
		float customop_hard_time = 0;

		std::string in_fpgaop = "cdma";
		std::string out_fpgaop = "cdma";
		std::string icore_fpgaop = "npu";
		std::string cpu_op = "Null";

		std::vector<std::tuple<std::string, float, float>> customops;
		std::map<std::string, float> customop_total_times;
		std::map<std::string, float> customop_hard_times;

		auto result = session.timeProfileResults();
		for (auto k_v : result)
		{
			// time1: total_time, time2: memcpy_time, time3: hard_time, time4: other_time
			auto &[time1, time2, time3, time4] = k_v.second;

			for (auto &[op, _, _1, _2, _3] : session.getForwards())
			{
				if (op->op_id == k_v.first)
				{
					auto op_typekey = op->typeKey();
					auto op_name = op->name;
					// Check if this op is io_process for ZG backend
					bool is_io_process = (op.getTag("io_process") == Bool(true));
					// is_io_process = std::find(io_process_list.begin(), io_process_list.end(), k_v.first) != io_process_list.end();

					// Write detailed timing info with is_io_process column for ZG backend

					ofs << fmt::format("op_id: {}, op_type: {}, op_name: {}, total_time: {}, memcpy_time: {}, hard_time: {}, other_time: {}, is_io_process: {}\n", k_v.first, op_typekey, op->name, time1, time2, time3, time4, is_io_process);

					total_time += time1;
					total_memcpy_time += time2;
					total_hard_time += time3;
					total_other_time += time4;
					// HardOp
					if (op_typekey == "icraft::xir::HardOpNode" && !is_io_process)
					{
						hardop_total_time += time1;
						hardop_memcpy_time += time2;
						hardop_hard_time += time3;
					}
					// is_io_process Op
					if (op_typekey == "icraft::xir::HardOpNode" && is_io_process)
					{
						io_total_time += time1;
						io_memcpytime += time2;
						io_hardtime += time3;
						io_othertime += time4;
					}
					// customOp
					if (op_typekey.find("customop") != std::string::npos)
					{
						if (op_typekey.find("ImageMake") != std::string::npos)
						{
							imk_on = true;
							icore_in_time += time3;
							in_fpgaop = "ImageMake";
						}
						else if (op_typekey.find("Post") != std::string::npos)
						{
							post_on = true;
							icore_out_time += time1;
							if (out_fpgaop == "cdma")
							{
								out_fpgaop = op_typekey.substr(0, op_typekey.size() - 4).substr(10);
							}
							else if (out_fpgaop.find(std::string(op_typekey.substr(0, op_typekey.size() - 4).substr(10))) == std::string::npos)
							{
								out_fpgaop = out_fpgaop + ";" + std::string(op_typekey.substr(0, op_typekey.size() - 4).substr(10));
							}
						}
						else
						{
							icore_time += time1;
							if (icore_fpgaop == "Null")
							{
								icore_fpgaop = std::string(op_typekey.substr(0, op_typekey.size() - 4).substr(10));
							}
							else if (icore_fpgaop.find(std::string(op_typekey.substr(0, op_typekey.size() - 4).substr(10))) == std::string::npos)
							{
								icore_fpgaop = icore_fpgaop + ";" + std::string(op_typekey.substr(0, op_typekey.size() - 4).substr(10));
							}
						}
						customop_total_time += time1;
						customop_hard_time += time3;
						if (customop_total_times.find(std::string(op_typekey)) != customop_total_times.end())
						{

							customop_total_times[std::string(op_typekey)] += time1;
							customop_hard_times[std::string(op_typekey)] += time3;
						}
						else
						{
							customop_total_times[std::string(op_typekey)] = time1;
							customop_hard_times[std::string(op_typekey)] = time3;
						}
					}
				}
			}
		}
		// this is different from BUYI, cuz no customop
		cpu_time = total_time - hardop_total_time - io_total_time;
		hardop_total_time -= hardop_memcpy_time;
		if (cpu_time < 0)
			cpu_time = 0;
		ofs << "************************************" << std::endl;
		ofs << fmt::format("Total_TotalTime: {}, Total_MemcpyTime: {}, Total_HardTime: {}, Total_OtherTime: {}\n",
						   total_time, total_memcpy_time, total_hard_time, total_other_time);
		ofs << fmt::format("Hardop_Total_Time: {} ms, Hardop_MemcpyTime: {} ms, Hardop_Hard_Time : {} ms.\n",
						   hardop_total_time, hardop_memcpy_time, hardop_hard_time);
		ofs << fmt::format("IO_TotalTime: {}, IO_MemcpyTime: {}, IO_HardTime: {}, IO_OtherTime: {}\n",
						   io_total_time, io_memcpytime, io_hardtime, io_othertime);

		std::cout << "\n"
				  << fmt::format("Total_TotalTime: {} ms, Total_MemcpyTime : {} ms, Total_HardTime : {} ms, Total_OtherTime : {} ms .",
								 total_time, total_memcpy_time, total_hard_time, total_other_time)
				  << std::endl;
		std::cout << fmt::format("Hardop_TotalTime: {} ms, Hardop_MemcpyTime: {} ms,Hardop_HardTime : {} ms.",
								 hardop_total_time, hardop_memcpy_time, hardop_hard_time)
				  << std::endl;
		if (post_on)
		{
			ofs << fmt::format("Customop_Total_Time: {} ms, Customop_Hard_Time : {} ms.", customop_total_time, customop_hard_time);
			std::cout << fmt::format("Customop_Total_Time: {} ms, Customop_Hard_Time : {} ms.", customop_total_time, customop_hard_time) << std::endl;
		}
		// std::cout << fmt::format("Customop_Total_Time: {} ms, Customop_Hard_Time : {} ms.",
		// 	customop_total_time, customop_hard_time) << std::endl;
		std::cout << fmt::format("IO_TotalTime: {} ms, IO_MemcpyTime : {} ms, IO_HardTime : {} ms, IO_OtherTime : {} ms .",
								 io_total_time, io_memcpytime, io_hardtime, io_othertime)
				  << std::endl;
		icore_time += hardop_total_time;
		io_process_time = io_total_time - io_memcpytime;
		ofs << "******************************************************\n";
		std::cout << "******************************************************\n";
		ofs << "统计分析结果如下(The analysis results are as follows):\n";
		std::cout << "统计分析结果如下(The analysis results are as follows):\n";
		ofs << "数据传入耗时(Data input time consumption):\n";
		std::cout << "数据传入耗时(Data input time consumption):\n";
		ofs << "Time(ms):" << io_memcpytime << "     Device:" << in_fpgaop << std::endl;
		std::cout << "Time(ms):" << io_memcpytime << "     Device:" << in_fpgaop << std::endl;
		ofs << "数据处理耗时(Data process time consumption):\n";
		std::cout << "数据处理耗时(Data process time consumption):\n";
		ofs << "Time(ms):" << io_process_time << "     Device:" << icore_fpgaop << std::endl;
		std::cout << "Time(ms):" << io_process_time << "     Device:" << icore_fpgaop << std::endl;
		ofs << "网络主体耗时(Network Backbone time-consuming):\n";
		std::cout << "网络主体耗时(Network Backbone time-consuming):\n";
		ofs << "Time(ms):" << icore_time << "     Device:" << icore_fpgaop << std::endl;
		std::cout << "Time(ms):" << icore_time << "     Device:" << icore_fpgaop << std::endl;
		if (post_on)
		{
			ofs << "后处理硬算子耗时(Post CustomOp time-consuming):\n";
			std::cout << "后处理硬算子耗时(Post CustomOp time-consuming):\n";
			ofs << "Time(ms):" << customop_total_time << "     Device:" << out_fpgaop << std::endl;
			std::cout << "Time(ms):" << customop_total_time << "     Device:" << out_fpgaop << std::endl;
		}
		ofs << "cpu算子耗时(CPU operator time consumption):\n";
		std::cout << "cpu算子耗时(CPU operator time consumption):\n";
		ofs << "Time(ms):" << cpu_time << "     Device:" << cpu_op << std::endl;
		std::cout << "Time(ms):" << cpu_time << "     Device:" << cpu_op << std::endl;
		ofs << "******************************************************\n";
		std::cout << "******************************************************\n";
		ofs << "PSIN模式主要用于部署调试、对齐精度等，用户仅需关注以下耗时即可，其余耗时大部分情况可以在实际的数据通路中被优化：" << std::endl;
		std::cout << "PSIN模式主要用于部署调试、对齐精度等，用户仅需关注以下耗时即可，其余耗时大部分情况可以在实际的数据通路中被优化：" << std::endl;
		ofs << "Hardop_HardTime(ms): " << hardop_hard_time << std::endl;
		std::cout << "Hardop_HardTime(ms): " << hardop_hard_time << std::endl;
		if (post_on)
		{
			ofs << "Customop_Hard_Time(ms): " << customop_hard_time << std::endl;
			std::cout << "Customop_Hard_Time(ms): " << customop_hard_time << std::endl;
		}
		std::cout << "******************************************************\n";
		ofs.close();

		std::cout << "For details about running time meassage of the network, check the " + network_name + "_time" + ".txt" + " in path: " + "./logs/" << std::endl;
	}
	else if (runBackend == "buyi")
	{ // Backend == buyi
		//--- Get network name and setup output file
		auto network_name = session->network_view.network()->name;
		checkDir("./logs/");
		std::string filePath = "./logs/" + network_name + "_time" + ".txt";
		std::ofstream ofs(filePath.c_str(), std::ios::out);

		float total_hard_time = 0;
		float total_time = 0;
		float total_memcpy_time = 0;
		float total_other_time = 0;
		float hardop_total_time = 0;
		float hardop_hard_time = 0;
		float hardop_memcpy_time = 0;

		bool imk_on = false;
		bool post_on = false;

		float out_cast_time = 0;
		float icore_in_time = 0;
		float icore_out_time = 0;
		float icore_time = 0;
		float cpu_time = 0;
		float customop_total_time = 0;
		float customop_hard_time = 0;
		std::string in_fpgaop = "cdma";
		std::string out_fpgaop = "cdma";
		std::string icore_fpgaop = "Null";
		std::string cpu_op = "Null";
		std::vector<std::tuple<std::string, float, float>> customops;
		std::map<std::string, float> customop_total_times;
		std::map<std::string, float> customop_hard_times;
		auto result = session.timeProfileResults();
		for (auto k_v : result)
		{
			// std::cout << k_v.first << std::endl;
			// std::cout << session->network_view.network().getOpById(k_v.first)->typeKey() << std::endl;
			// std::cout << session->network_view.network().getOpById(k_v.first)->name << std::endl;
			auto &[time1, time2, time3, time4] = k_v.second;

			for (auto &[op, _, _1, _2, _3] : session.getForwards())
			{
				if (op->op_id == k_v.first)
				{
					auto op_typekey = op->typeKey();
					auto op_name = op->name;
					// std::cout << op->typeKey() << std::endl;
					// std::cout << op->name << std::endl;
					ofs << fmt::format("op_id: {}, op_type: {}, op_name: {}, total_time: {}, memcpy_time: {}, hard_time: {}, other_time: {}\n",
									   k_v.first, op_typekey, op->name, time1, time2, time3, time4);
					total_time += time1;
					total_memcpy_time += time2;
					total_hard_time += time3;
					total_other_time += time4;
					if (op_typekey == "icraft::xir::HardOpNode")
					{
						hardop_total_time += time1;
						// hardop_total_time -= time2;
						hardop_memcpy_time += time2;
						hardop_hard_time += time3;
					}
					if (op_typekey == "icraft::xir::CastNode")
					{
						if (time2 > 0.001)
						{
							out_cast_time += time2;
						}
					}
					if (op_typekey.find("customop") != std::string::npos)
					{
						if (op_typekey.find("ImageMake") != std::string::npos)
						{
							imk_on = true;
							icore_in_time += time3;
							in_fpgaop = "ImageMake";
						}
						else if (op_typekey.find("Post") != std::string::npos)
						{
							post_on = true;
							icore_out_time += time1;
							if (out_fpgaop == "cdma")
							{
								out_fpgaop = op_typekey.substr(0, op_typekey.size() - 4).substr(10);
							}
							else if (out_fpgaop.find(std::string(op_typekey.substr(0, op_typekey.size() - 4).substr(10))) == std::string::npos)
							{
								out_fpgaop = out_fpgaop + ";" + std::string(op_typekey.substr(0, op_typekey.size() - 4).substr(10));
							}
						}
						else
						{
							icore_time += time1;
							if (icore_fpgaop == "Null")
							{
								icore_fpgaop = std::string(op_typekey.substr(0, op_typekey.size() - 4).substr(10));
							}
							else if (icore_fpgaop.find(std::string(op_typekey.substr(0, op_typekey.size() - 4).substr(10))) == std::string::npos)
							{
								icore_fpgaop = icore_fpgaop + ";" + std::string(op_typekey.substr(0, op_typekey.size() - 4).substr(10));
							}
						}
						customop_total_time += time1;
						customop_hard_time += time3;
						if (customop_total_times.find(std::string(op_typekey)) != customop_total_times.end())
						{

							customop_total_times[std::string(op_typekey)] += time1;
							customop_hard_times[std::string(op_typekey)] += time3;
						}
						else
						{
							customop_total_times[std::string(op_typekey)] = time1;
							customop_hard_times[std::string(op_typekey)] = time3;
						}
					}
				}
			}
			// ofs << fmt::format("op_id: {}, op_type: {}, op_name: {}, total_time: {}, memcpy_time: {}, hard_time: {}, other_time: {}\n",
			//    k_v.first, session->network_view.network().getOpById(k_v.first)->typeKey(), session->network_view.network().getOpById(k_v.first)->name
			//    , time1, time2, time3, time4);
		}
		if (!post_on)
		{
			icore_out_time = out_cast_time;
			cpu_time = total_time - hardop_total_time - customop_total_time - icore_out_time;
		}
		else
		{
			cpu_time = total_time - hardop_total_time - customop_total_time;
		}
		if (!imk_on)
		{
			hardop_total_time -= hardop_memcpy_time;
			icore_in_time = hardop_memcpy_time;
		}

		if (cpu_time < 0)
			cpu_time = 0;
		ofs << "************************************" << std::endl;
		ofs << fmt::format("Total_TotalTime: {}, Total_MemcpyTime: {}, Total_HardTime: {}, Total_OtherTime: {}\n",
						   total_time, total_memcpy_time, total_hard_time, total_other_time);
		ofs << fmt::format("Hardop_Total_Time: {} ms, Hardop_Hard_Time : {} ms.\n",
						   hardop_total_time, hardop_hard_time);
		// ofs << fmt::format("Customop_Total_Time: {} ms, Customop_Hard_Time : {} ms.",
		// 	customop_total_time, customop_hard_time);

		std::cout << "\n"
				  << fmt::format("Total_TotalTime: {} ms, Total_MemcpyTime : {} ms, Total_HardTime : {} ms, Total_OtherTime : {} ms .",
								 total_time, total_memcpy_time, total_hard_time, total_other_time)
				  << std::endl;
		std::cout << fmt::format("Hardop_TotalTime: {} ms, Hardop_HardTime : {} ms.",
								 hardop_total_time, hardop_hard_time)
				  << std::endl;
		// std::cout << fmt::format("Customop_Total_Time: {} ms, Customop_Hard_Time : {} ms.",
		// 	customop_total_time, customop_hard_time) << std::endl;
		icore_time += hardop_total_time;
		for (const auto &pair : customop_total_times)
		{

			ofs << fmt::format("Customop: {}, TotalTime: {} ms, HardTime : {} ms.\n",
							   pair.first.substr(0, pair.first.size() - 4).substr(10), pair.second, customop_hard_times[pair.first]);
			std::cout << fmt::format("Customop: {}, TotalTime: {} ms, HardTime : {} ms.",
									 pair.first.substr(0, pair.first.size() - 4).substr(10), pair.second, customop_hard_times[pair.first])
					  << std::endl;
		}
		ofs << "******************************************************\n";
		std::cout << "******************************************************\n";
		ofs << "统计分析结果如下(The analysis results are as follows):\n";
		std::cout << "统计分析结果如下(The analysis results are as follows):\n";
		ofs << "数据传入耗时(Data input time consumption):\n";
		std::cout << "数据传入耗时(Data input time consumption):\n";
		ofs << "Time(ms):" << icore_in_time << "     Device:" << in_fpgaop << std::endl;
		std::cout << "Time(ms):" << icore_in_time << "     Device:" << in_fpgaop << std::endl;
		ofs << "icore[npu]耗时(Icore [npu] time-consuming):\n";
		std::cout << "icore[npu]耗时(Icore [npu] time-consuming):\n";
		ofs << "Time(ms):" << icore_time << "     Device:" << icore_fpgaop << std::endl;
		std::cout << "Time(ms):" << icore_time << "     Device:" << icore_fpgaop << std::endl;
		ofs << "数据传出耗时(Data output time consumption):\n";
		std::cout << "数据传出耗时(Data output time consumption):\n";
		ofs << "Time(ms):" << icore_out_time << "     Device:" << out_fpgaop << std::endl;
		std::cout << "Time(ms):" << icore_out_time << "     Device:" << out_fpgaop << std::endl;
		ofs << "cpu算子耗时(CPU operator time consumption):\n";
		std::cout << "cpu算子耗时(CPU operator time consumption):\n";
		ofs << "Time(ms):" << cpu_time << "     Device:" << cpu_op << std::endl;
		std::cout << "Time(ms):" << cpu_time << "     Device:" << cpu_op << std::endl;
		ofs << "******************************************************\n";
		std::cout << "******************************************************\n";
		ofs << "PSIN模式主要用于部署调试、对齐精度等，用户仅需关注以下耗时即可，其余耗时大部分情况可以在实际的数据通路中被优化：" << std::endl;
		std::cout << "PSIN模式主要用于部署调试、对齐精度等，用户仅需关注以下耗时即可，其余耗时大部分情况可以在实际的数据通路中被优化：" << std::endl;
		ofs << "Hardop_HardTime(ms): " << hardop_hard_time << std::endl;
		std::cout << "Hardop_HardTime(ms): " << hardop_hard_time << std::endl;
		for (const auto &pair : customop_total_times)
		{

			if (pair.first.find("ImageMake") != std::string::npos) continue;
			ofs << fmt::format("Customop: {}, HardTime(ms): {}\n",
							   pair.first.substr(0, pair.first.size() - 4).substr(10), customop_hard_times[pair.first]);
			std::cout << fmt::format("Customop: {}, HardTime(ms): {}",
									 pair.first.substr(0, pair.first.size() - 4).substr(10), customop_hard_times[pair.first])
					  << std::endl;
		}
		std::cout << "******************************************************\n";
		ofs.close();

		std::cout << "For details about running time meassage of the network, check the " + network_name + "_time" + ".txt" + " in path: " + "./logs/" << std::endl;
	}
	else
	{
		std::cout << fmt::format("Not Supported Backend: {}", runBackend) << std::endl;
	}
};

inline void checkBackend(const std::string &run_backend)
{
	if (run_backend.compare("host") == 0)
		;
	else if (run_backend.compare("buyi") == 0)
		;
	else if (run_backend.compare("zg330") == 0)
		;
	else
	{
		ICRAFT_LOG(EXCEPT).append("The backend parameter passed to function openDevice <{}> is not supported.\
			\nEnsure that you pass the correct backend parameter!\
			\nThe backend parameter can only accept host, buyi, and zg330!",
								  run_backend);
	}
}

inline icraft::xrt::Device openDevice(const std::string &run_backend, const std::string &ip,
									  bool mmu_Mode = true, bool cuda_Mode = false,
									  std::string npu_addr = "0x40000000", std::string dma_addr = "0x80000000")
{
#ifdef _WIN32
	if (run_backend.compare("host") == 0)
	{
		if (cuda_Mode)
		{
			return icraft::xrt::CudaDevice::Default();
			;
		}
		return icraft::xrt::HostDevice::Default();
	}
	else if (run_backend.compare("buyi") == 0)
	{
		std::string URL_PATH = "socket://ql100aiu@" + ip + ":9981?npu=" + npu_addr + "&dma=" + dma_addr;
		icraft::xrt::Device device;
		device = icraft::xrt::Device::Open(URL_PATH);
		device.cast<icraft::xrt::BuyiDevice>().mmuModeSwitch(mmu_Mode);
		return device;
	}
	else if (run_backend.compare("zg330") == 0)
	{
		std::string URL_PATH = "socket://zg330aiu@" + ip + ":9981";
		icraft::xrt::Device device = icraft::xrt::Device::Open(URL_PATH);
		return device;
	}
	else
	{
		ICRAFT_LOG(EXCEPT).append("The backend parameter passed to function openDevice <{}> is not supported.\
			\nEnsure that you pass the correct backend parameter!\
			\nThe backend parameter can only accept host, buyi, and zg330!",
								  run_backend);
		return icraft::xrt::Device{};
	}

#elif __linux__
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
	// std::cout << "x86_64 (Intel/AMD 64-bit) or x86 (32-bit)" << std::endl;
	if (run_backend.compare("host") == 0)
	{
		if (cuda_Mode)
		{
			return icraft::xrt::CudaDevice::Default();
			;
		}
		return icraft::xrt::HostDevice::Default();
	}
	else if (run_backend.compare("buyi") == 0)
	{
		std::string URL_PATH = "socket://ql100aiu@" + ip + ":9981?npu=" + npu_addr + "&dma=" + dma_addr;
		icraft::xrt::Device device;
		device = icraft::xrt::Device::Open(URL_PATH);
		device.cast<icraft::xrt::BuyiDevice>().mmuModeSwitch(mmu_Mode);
		return device;
	}
	else if (run_backend.compare("zg330") == 0)
	{
		std::string URL_PATH = "socket://zg330aiu@" + ip + ":9981";
		icraft::xrt::Device device = icraft::xrt::Device::Open(URL_PATH);
		return device;
	}
	else
	{
		ICRAFT_LOG(EXCEPT).append("The backend parameter passed to function openDevice <{}> is not supported.\
				\nEnsure that you pass the correct backend parameter!\
				\nThe backend parameter can only accept host, buyi, and zg330!",
								  run_backend);
	}
#elif defined(__aarch64__) || defined(_M_ARM64)
	// std::cout << "AArch64 (ARM 64-bit)" << std::endl;
	if (run_backend.compare("buyi") == 0)
	{
		std::string URL_PATH = "axi://ql100aiu?npu=" + npu_addr + "&dma=" + dma_addr;
		icraft::xrt::Device device;
		device = icraft::xrt::Device::Open(URL_PATH);
		device.cast<icraft::xrt::BuyiDevice>().mmuModeSwitch(mmu_Mode);
		return device;
	}
	else if (run_backend.compare("zg330") == 0)
	{
		std::string URL_PATH = "axi://zg330aiu?npu=" + npu_addr + "&dma=" + dma_addr;
		icraft::xrt::Device device;
		device = icraft::xrt::Device::Open(URL_PATH);
		return device;
	}
	else
	{
		ICRAFT_LOG(EXCEPT).append("The backend parameter passed to function openDevice <{}> is not supported.\
				\nEnsure that you pass the correct backend parameter!\
				\nThe backend parameter can only accept host, buyi, and zg330!",
								  run_backend);
		return icraft::xrt::Device{};
	}
#else
	std::cout << "Unknown architecture" << std::endl;
#endif
#endif
}

inline std::pair<std::string, std::string> getJrPath(const std::string &run_backend, const std::string &folderPath, std::string &targetFileName)
{
#if defined(_WIN32) || defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
	if (run_backend.compare("buyi") == 0)
	{
		targetFileName = "BY.json";
	}
	else if (run_backend.compare("zg330") == 0)
	{
		targetFileName = "ZG.json";
	}
	else if (run_backend.compare("host") == 0)
	{
		if (STAGE.count(targetFileName) > 0)
			if (targetFileName.compare("g") == 0)
			{
				for (const auto &entry : std::filesystem::directory_iterator(folderPath))
				{
					// std::cout << entry.path().filename().string() << std::endl;
					if (entry.is_regular_file() && entry.path().filename().string().find("BY.json") != std::string::npos)
					{
						spdlog::info("Found model file at:{}", entry.path().string());
						std::regex regex_last("json(?!.*json)", std::regex::icase);
						std::string raw_path = std::regex_replace(entry.path().string(), regex_last, "raw");
						return {entry.path().string(), raw_path};
					}
					if (entry.is_regular_file() && entry.path().filename().string().find("ZG.json") != std::string::npos)
					{
						spdlog::info("Found model file at:{}", entry.path().string());
						std::regex regex_last("json(?!.*json)", std::regex::icase);
						std::string raw_path = std::regex_replace(entry.path().string(), regex_last, "raw");
						return {entry.path().string(), raw_path};
					}
				}
				throw std::runtime_error("imodel path not right ,please check yaml:imodel:dir");
			}
			else
			{
				targetFileName = STAGE[targetFileName] + ".json";
			}
		else
			throw std::runtime_error("imodel stage not right ,please check yaml:imodel:dir");
	}
	else
	{
		ICRAFT_LOG(EXCEPT).append("The backend parameter passed to function getJrPath <{}> is not supported.\
			\nEnsure that you pass the correct backend parameter!\
			\nThe backend parameter can only accept host, buyi, and zg330!",
								  run_backend);
	}

#elif defined(__aarch64__) || defined(_M_ARM64)
	if (run_backend.compare("buyi") == 0)
	{
		targetFileName = "BY.json";
	}
	else if (run_backend.compare("zg330") == 0)
	{
		targetFileName = "ZG.json";
	}
	else
	{
		ICRAFT_LOG(EXCEPT).append("The backend parameter passed to function getJrPath <{}> is not supported.\
			\nEnsure that you pass the correct backend parameter!\
			\nThe backend parameter can only accept buyi and zg330!",
								  run_backend);
	}
#else
	std::cout << "Unknown architecture" << std::endl;
#endif
	for (const auto &entry : std::filesystem::directory_iterator(folderPath))
	{
		// std::cout << entry.path().filename().string() << std::endl;
		if (entry.is_regular_file() && entry.path().filename().string().find(targetFileName) != std::string::npos)
		{
			spdlog::info("Found model file at:{}", entry.path().string());

			std::regex regex_last("json(?!.*json)", std::regex::icase);
			std::string raw_path = std::regex_replace(entry.path().string(), regex_last, "raw");

			return {entry.path().string(), raw_path};
		}
	}
	throw std::runtime_error("imodel path not right ,please check yaml:imodel:dir");
}

inline icraft::xir::Network loadNetwork(const std::string &JSON_PATH, const std::string &RAW_PATH)
{
	icraft::xir::Network network = icraft::xir::Network::CreateFromJsonFile(JSON_PATH);
	network.lazyLoadParamsFromFile(RAW_PATH);
	return network;
}

inline icraft::xrt::Session initSession(const std::string &run_backend, const icraft::xrt::NetworkView &network, icraft::xrt::Device &device,
										int ocm_option = -1, bool mmuMode = true, bool open_speedmode = true, bool open_compressFtmp = true)
{
	if (run_backend.compare("host") == 0)
	{
		auto session = icraft::xrt::Session::Create<icraft::xrt::HostBackend>(network, {device});
		return session;
	}
	else if (run_backend.compare("buyi") == 0)
	{
		auto session = icraft::xrt::Session::Create<icraft::xrt::BuyiBackend, icraft::xrt::HostBackend>(network, {device, icraft::xrt::HostDevice::Default()});
		if (mmuMode)
			return session;
		auto buyi_backend = session->backends[0].cast<icraft::xrt::BuyiBackend>();
		if (open_compressFtmp)
			buyi_backend.compressFtmp();
		if (open_speedmode)
			buyi_backend.speedMode();
		return session;
	}
	else if (run_backend.compare("zg330") == 0)
	{
		auto session = icraft::xrt::Session::Create<icraft::xrt::zg330::ZG330Backend, icraft::xrt::HostBackend>(network, {device, icraft::xrt::HostDevice::Default()});

		auto zg_backend = session->backends[0].cast<icraft::xrt::zg330::ZG330Backend>();
		if (!open_compressFtmp)
			zg_backend.disableEtmOptimize();
		if (!open_speedmode)
			zg_backend.disableMergeHardOp();
		if (ocm_option != -1)
		{
			if (ocm_option == 3)
			{
				zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION3);
			}
			else if (ocm_option == 2)
			{
				zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION2);
			}
			else if (ocm_option == 1)
			{
				zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION1);
			}
			else if (ocm_option == 0)
			{
				zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::None);
			}
			else
			{
				ICRAFT_LOG(EXCEPT).append("The ocm_option parameter passed to initSession <{}> is not supported.\
			\nEnsure that you pass the correct ocm_option parameter!\
			\nThe backend parameter can only accept 0, 1, 2, 3 and -1!",
										  ocm_option);
			}
		}

		return session;
	}
	else
	{
		ICRAFT_LOG(EXCEPT).append("The backend parameter passed to function initSession <{}> is not supported.\
			\nEnsure that you pass the correct backend parameter!\
			\nThe backend parameter can only accept host, buyi, and zg330!",
								  run_backend);
		return icraft::xrt::Session{};
	}
}

icraft::xrt::Tensor CvMat2Tensor(cv::Mat &img, const icraft::xrt::Network &network)
{
	// 获取输入的value 用于从 cvMat 构造 输入tensor
	auto input_value = network.inputs()[0];
	// 将cv Mat构造为输入网络的TENSOR
	auto out_dtype = input_value.tensorType().clone();
	auto out_stor_type = out_dtype->element_dtype.getStorageType();
	cv::Mat converted;
	if (out_stor_type.is<icraft::xir::FloatType>())
	{
		auto float_stor_type = out_stor_type.cast<icraft::xir::FloatType>();
		if (float_stor_type.isFP32())
		{
			img.convertTo(converted, CV_32F);
		}
		else if (float_stor_type.isFP16())
		{
			img.convertTo(converted, CV_16F);
		}
		else
		{
			ICRAFT_LOG(EXCEPT).append("[Error in HostBackend Image2Tensor] DataType {} is not supported.", float_stor_type->typeKey());
		}
	}
	else if (out_stor_type.is<icraft::xir::IntegerType>())
	{
		auto int_stor_type = out_stor_type.cast<icraft::xir::IntegerType>();
		if (int_stor_type.isSInt8())
		{
			img.convertTo(converted, CV_8S);
		}
		else if (int_stor_type.isUInt8())
		{
			img.convertTo(converted, CV_8U);
		}
		else if (int_stor_type.isSInt16())
		{
			img.convertTo(converted, CV_16S);
		}
		else if (int_stor_type.isUInt16())
		{
			img.convertTo(converted, CV_16U);
		}
		else if (int_stor_type.isSInt32())
		{
			img.convertTo(converted, CV_32S);
		}
		else
		{
			ICRAFT_LOG(EXCEPT).append("[Error in HostBackend Image2Tensor] DataType {} is not supported.", int_stor_type->typeKey());
		}
	}
	else
	{
		ICRAFT_LOG(EXCEPT).append("[Error in HostBackend Image2Tensor] DataType {} is not supported.", out_stor_type->typeKey());
	}
	int H = converted.rows;
	int W = converted.cols;
	int C = converted.channels();
	// define output tensor
	std::vector<int64_t> output_shape = {1, H, W, C};
	auto tensor_layout = icraft::xir::Layout("NHWC");
	out_dtype.setShape(output_shape);
	icraft::xrt::Tensor img_tensor = icraft::xrt::Tensor(out_dtype).mallocOn(icraft::xrt::HostDevice::MemRegion());
	// data copy
	memcpy(img_tensor.data().cptr(), converted.data, H * W * C * out_dtype->element_dtype.bits() / 8);
	// std::cout << "CvMat2Tensor: " << img_tensor.dtype()->shape << std::endl;
	return img_tensor;
}

template <typename T>
icraft::xrt::Tensor data2Tensor(const T *input_data, const icraft::xir::Value &input_value)
{
	icraft::xir::TensorType out_dtype;
	if (input_value.tensorType()->shape[0] == -1)
	{
		out_dtype = input_value.getUsesOp()[0]->outputs[0].tensorType().clone();
	}
	else
	{
		out_dtype = input_value.tensorType().clone();
	}
	auto size = out_dtype.numElements();

	auto out_stor_type = out_dtype->element_dtype.getStorageType();

	auto ele_dtype = out_dtype->element_dtype;

	if (ele_dtype.isUInt(8))
	{
		auto param_chunk = icraft::xrt::HostDevice::MemRegion().malloc(size * sizeof(uint8_t)); // malloc on host
		auto trans_data = (uint8_t *)param_chunk->begin.cptr();
		std::transform((T *)input_data, (T *)input_data + size, trans_data, [](auto d)
					   { return (uint8_t)d; });
		return icraft::xrt::Tensor(out_dtype, param_chunk);
	}
	else if (ele_dtype.isSInt(8))
	{
		auto param_chunk = icraft::xrt::HostDevice::MemRegion().malloc(size * sizeof(int8_t)); // malloc on host
		auto trans_data = (int8_t *)param_chunk->begin.cptr();
		std::transform((T *)input_data, (T *)input_data + size, trans_data, [](auto d)
					   { return (int8_t)d; });
		return icraft::xrt::Tensor(out_dtype, param_chunk);
	}
	else if (ele_dtype.isUInt(16))
	{
		auto param_chunk = icraft::xrt::HostDevice::MemRegion().malloc(size * sizeof(uint16_t)); // malloc on host
		auto trans_data = (uint16_t *)param_chunk->begin.cptr();
		std::transform((T *)input_data, (T *)input_data + size, trans_data, [](auto d)
					   { return (uint16_t)d; });
		return icraft::xrt::Tensor(out_dtype, param_chunk);
	}
	else if (ele_dtype.isSInt(16))
	{
		auto param_chunk = icraft::xrt::HostDevice::MemRegion().malloc(size * sizeof(int16_t)); // malloc on host
		auto trans_data = (int16_t *)param_chunk->begin.cptr();
		std::transform((T *)input_data, (T *)input_data + size, trans_data, [](auto d)
					   { return (int16_t)d; });
		return icraft::xrt::Tensor(out_dtype, param_chunk);
	}
	else if (ele_dtype.isUInt(32))
	{
		auto param_chunk = icraft::xrt::HostDevice::MemRegion().malloc(size * sizeof(uint32_t)); // malloc on host
		auto trans_data = (uint32_t *)param_chunk->begin.cptr();
		std::transform((T *)input_data, (T *)input_data + size, trans_data, [](auto d)
					   { return (uint32_t)d; });
		return icraft::xrt::Tensor(out_dtype, param_chunk);
	}
	else if (ele_dtype.isSInt(32))
	{
		auto param_chunk = icraft::xrt::HostDevice::MemRegion().malloc(size * sizeof(int32_t)); // malloc on host
		auto trans_data = (int32_t *)param_chunk->begin.cptr();
		std::transform((T *)input_data, (T *)input_data + size, trans_data, [](auto d)
					   { return (int32_t)d; });
		return icraft::xrt::Tensor(out_dtype, param_chunk);
	}
	else if (ele_dtype.isFP32())
	{
		auto param_chunk = icraft::xrt::HostDevice::MemRegion().malloc(size * sizeof(float)); // malloc on host
		auto trans_data = (float *)param_chunk->begin.cptr();
		std::transform((T *)input_data, (T *)input_data + size, trans_data, [](auto d)
					   { return (float)d; });
		return icraft::xrt::Tensor(out_dtype, param_chunk);
	}
	else
	{
		ICRAFT_LOG(EXCEPT).append("[Error in HostBackend::GenTensorFromParams] Unsupported dtype {}, can't convert to torch tensor.", ele_dtype->typeKey());
		return icraft::xrt::Tensor{};
	}
}

/*----------- Debug Helpers ---------------*/
inline void dumpOutputFtmp(icraft::xir::NetworkView network, std::vector<icraft::xrt::Tensor> &output_tensors, std::string dump_format, std::string log_path)
{
	std::filesystem::create_directories(log_path);
	auto network_outp = network.outputs();
	// dump网络output算子的输出
	for (uint64_t i = 0; i < network_outp.size(); i++)
	{
		// auto os = std::ofstream(fmt::format("{}//{}.ftmp", log_path, network_outp[i]->v_id), std::ios::binary);//存实际ftmp_id
		auto os = std::ofstream(fmt::format("{}//{}.ftmp", log_path, std::to_string(i)), std::ios::binary); // 存输出顺序
		output_tensors[i].dump(os, dump_format);
	}
};

inline void replaceFtmp(const icraft::xrt::BuyiBackend &by_backend, int v_id, std::string ftmp_path)
{
	throw std::runtime_error("replaceFtmp not support BuyiBackend yet.");
}

inline void replaceFtmp(const icraft::xrt::zg330::ZG330Backend &zg_backend, int v_id, std::string ftmp_path)
{
	if (!std::filesystem::exists(ftmp_path))
	{
		std::cout << "ftmp path: " << ftmp_path << " not exist, skip replace ftmp." << std::endl;
		return;
	}
	auto memchunk_map = zg_backend->forward_info->memchunk_map;
	auto value_map = zg_backend->forward_info->value_map;
	auto v_info = value_map.at(v_id);
	if (v_info->is_host || v_info->is_ocm)
	{
		std::cout << "Value id: " << v_info->value->v_id << " is host or ocm, skip replace ftmp." << std::endl;
		return;
	}
	auto id = v_info->value->v_id;
	std::cout << "start replace ftmp for v_id=" << v_id << ", actual id=" << id << std::endl;
	auto byte_size = v_info->byte_size;
	auto phy_addr = v_info->phy_addr;
	auto mem_chunk = memchunk_map.at(id)->memChunk;
	std::cout << "start dump v_id: " << id << ", phy addr: " << phy_addr << ", mem chunk begin addr: " << mem_chunk->begin.addr() << std::endl;
	std::shared_ptr<int8_t[]> temp = std::shared_ptr<int8_t[]>(new int8_t[byte_size]{0});
	std::ifstream in_file(ftmp_path, std::ios::in | std::ios::binary);
	in_file.read(reinterpret_cast<char *>(temp.get()), byte_size);
	in_file.close();
	mem_chunk.write(phy_addr - mem_chunk->begin.addr(), reinterpret_cast<char *>(temp.get()), byte_size);
}

/*-------------------------------------*/
// ZG version of dump hidden ftmps
inline void dumpFtmps(const std::string &network_name, const icraft::xrt::zg330::ZG330Backend &zg_backend)
{
	auto file_name = "./logs/" + network_name;
	if (!std::filesystem::is_directory(file_name) || !std::filesystem::exists(file_name))
	{
		std::filesystem::create_directories(file_name);
	}

	auto memchunk_map = zg_backend->forward_info->memchunk_map;
	auto value_map = zg_backend->forward_info->value_map;

	for (auto &&[_, v_info] : value_map)
	{
		if (v_info->is_host || v_info->is_ocm)
		{
			continue;
		}
		// if (v_info->segment == Segment::FTMP) continue;
		auto id = v_info->value->v_id;
		auto byte_size = v_info->byte_size;
		auto phy_addr = v_info->phy_addr;
		auto mem_chunk = memchunk_map.at(id)->memChunk;
		std::cout << "start dump v_id: " << id << ", phy addr: " << phy_addr << ", mem chunk begin addr: " << mem_chunk->begin.addr() << std::endl;
		std::shared_ptr<int8_t[]> temp = std::shared_ptr<int8_t[]>(new int8_t[byte_size]{0});
		mem_chunk.read(reinterpret_cast<char *>(temp.get()), phy_addr - mem_chunk->begin.addr(), byte_size);
		std::ofstream out_file(file_name + "/" + std::to_string(id) + ".ftmp", std::ios::out | std::ios::binary);
		out_file.write(reinterpret_cast<char *>(temp.get()), byte_size);
		out_file.close();
	}
}

// BY version of dump hidden ftmps
inline void dumpFtmps(const std::string &network_name, const icraft::xrt::BuyiBackend &buyi_backend)
{
	auto file_name = "./logs/" + network_name;
	if (!std::filesystem::is_directory(file_name) || !std::filesystem::exists(file_name))
	{
		std::filesystem::create_directories(file_name);
	}

	auto phy_segment_map = buyi_backend->phy_segment_map;
	auto value_map = buyi_backend->forward_info->value_map;

	for (auto k_v : value_map)
	{
		if (k_v.second->is_host || k_v.second->is_ocm)
		{
			continue;
		}
		// if (k_v.second->segment == Segment::FTMP) continue;
		auto id = k_v.second->value->v_id;
		auto byte_size = k_v.second->byte_size;
		auto phy_addr = k_v.second->phy_addr;
		auto mem_chunk = phy_segment_map.at(k_v.second->segment)->memchunk;
		std::cout << "start dump v_id: " << id << ", phy addr: " << phy_addr << ", mem chunk begin addr: " << mem_chunk->begin.addr() << std::endl;
		std::shared_ptr<int8_t[]> temp = std::shared_ptr<int8_t[]>(new int8_t[byte_size]{0});
		mem_chunk.read(reinterpret_cast<char *>(temp.get()), phy_addr - mem_chunk->begin.addr(), byte_size);
		std::ofstream out_file(file_name + "/" + std::to_string(id) + ".ftmp", std::ios::out | std::ios::binary);
		out_file.write(reinterpret_cast<char *>(temp.get()), byte_size);
		out_file.close();
	}
}

template <typename BackendType>
inline void listIOProcessOps(icraft::xrt::Session &net_sess)
{
	std::cout << "io_process operators:" << std::endl;
	for (auto &[op, backend, _1, _2, _3] : net_sess.getForwards())
	{
		auto op_id = op->op_id;
		auto op_typekey = op->typeKey();
		auto op_name = op->name;
		bool is_device_backend = backend->template is<BackendType>();
		bool is_io_process = op.getTag("io_process") == icraft::xir::Bool(true);
		if (is_io_process)
			std::cout << op_id << "," << op_typekey << "," << op_name << ", is on device_backend:" << is_device_backend << std::endl;
	}
	std::cout << std::endl;
}

// Buyi version of dumping imagemake output tensor as image
inline void dumpImkOutAsImage(const icraft::xrt::Tensor &imk_out_tensor, const std::string &log_path, int imk_port = 0, int channel_bits = 8)
{
	std::filesystem::create_directories(log_path);
	auto imk_host_tensor = imk_out_tensor.to(icraft::xrt::HostDevice::MemRegion());
	cv::Mat output_img;
	auto shape = imk_host_tensor.dtype()->shape;
	int H = shape[2];
	int W = shape[3];
	int C = shape[4];
	spdlog::debug("imk_out tensor shape: N:{} -:{} H:{} W:{} C:{}", shape[0], shape[1], shape[2], shape[3], shape[4]);
	auto tensor_data = (uint8_t *)(imk_host_tensor.data().cptr());
	if (channel_bits == 8 && C == 4)
	{
		cv::Mat imk_out = cv::Mat(H, W, CV_8SC4, tensor_data);
		cv::Mat imk_out_unsigned;
		imk_out.convertTo(imk_out_unsigned, CV_8UC4, 1.0, 127.0);
		cv::cvtColor(imk_out_unsigned, output_img, cv::COLOR_RGBA2RGB);
		cv::cvtColor(output_img, output_img, cv::COLOR_RGB2BGR);
	}
	else if (channel_bits == 16 && C == 1)
	{
		cv::Mat imk_out = cv::Mat(H, W, CV_16SC1, tensor_data);
		imk_out.convertTo(imk_out, CV_16FC1);
		imk_out.convertTo(output_img, CV_8UC3);
	}
	else
	{
		ICRAFT_LOG(EXCEPT).append("[Error in dumpImkOutAsImage] DataType is not supported.");
		return;
	}
	auto time_stamp = get_timestamp_string();
	cv::imwrite(fmt::format("{}/imk{}_out_{}.png", log_path, imk_port, time_stamp), output_img);
	std::ofstream out_file(fmt::format("{}/imk{}_out_{}.ftmp", log_path, imk_port, time_stamp), std::ios::out | std::ios::binary);
	out_file.write(reinterpret_cast<char *>(tensor_data), H * W * C * (channel_bits / 8));
	out_file.close();
}

inline void dumpImkOutAsImage(icraft::xrt::Device device, uint64_t pl_addr, int width, int height, int chn = 3,
							  const std::string &dump_path = "io/output/imagemake", const std::string &runBackend = "buyi", const std::string &prefix = "")
{
	if (runBackend != "buyi" && runBackend != "zg330")
	{
		ICRAFT_LOG(EXCEPT).append("[Error in dumpImkOutAsImage] Unsupported runBackend {}, only support buyi and zg330.", runBackend);
		return;
	}
	if (runBackend == "buyi" && chn != 4)
	{
		ICRAFT_LOG(EXCEPT).append("[Error in dumpImkOutAsImage] Buyi imagemake out only supports 4-channel output.");
		return;
	}
	if (runBackend == "zg330" && chn != 3)
	{
		ICRAFT_LOG(EXCEPT).append("[Error in dumpImkOutAsImage] ZG330 imagemake out only supports 3-channel output.");
		return;
	}
	std::filesystem::create_directories(dump_path);
	auto IMAGEMAKE_OUTPUT_SIZE = width * height * chn; // RGBA
	auto imk_readout_data = new uint8_t[IMAGEMAKE_OUTPUT_SIZE];
	icraft::xrt::MemPtr chunk_ptr(pl_addr);
	device.defaultMemRegion().read((char *)imk_readout_data, chunk_ptr, IMAGEMAKE_OUTPUT_SIZE);
	cv::Mat output_img;
	auto fn = fmt::format("{}/{}_imkout_{}_{}.png", dump_path, prefix, runBackend, get_timestamp_string());
	if (runBackend == "buyi")
	{
		cv::Mat imk_out = cv::Mat(height, width, CV_8SC4, imk_readout_data);
		cv::Mat imk_out_unsigned;
		imk_out.convertTo(imk_out_unsigned, CV_8UC4, 1.0, 127.0);
		cv::cvtColor(imk_out_unsigned, output_img, cv::COLOR_BGRA2BGR);
		LOG_INFO("[dumpImkOutAsImage]", "buyi imagemake out 保存到{}，从8SC4转到8UC4，然后BGRA转BGR，通道顺序是BGR。", fn);
	}
	else
	{
		cv::Mat imk_out = cv::Mat(height, width, CV_8SC3, imk_readout_data);
		imk_out.convertTo(output_img, CV_8UC3, 1.0, 127.0);
		LOG_INFO("[dumpImkOutAsImage]", "zg330 imagemake out 保存到{}，从8SC3转到8UC3，通道顺序是BGR。", fn);
	}
	cv::imwrite(fn, output_img);
	delete imk_readout_data;
}

// 删除输出分支上的指定pattern（cast-Pruneaxis），并按照原来output算子的ifm顺序重新连接hardop <->output；
// idx_list用于指定分支删除cast&Pruneaxis算子，例如：指定第1条分支删除cast&Pruneaxis算子：idx_list={0}
void removeOutputCast(icraft::xir::Network &network, bool mmu, icraft::xir::Array<int> idx_list = {})
{
	auto codegen_speedmode = Downcast<icraft::xir::Bool>(network.getTag("speedmode").value())->value;
	auto codegen_compressFtmp = Downcast<icraft::xir::Bool>(network.getTag("compressFtmp").value())->value;
	bool codegen_mmu = codegen_speedmode || codegen_compressFtmp;
	if (codegen_mmu || mmu)
		ICRAFT_LOG(WARNING).append("Open MMU will lock the order of ftmp's physical address, and this may affect network connection!");

	auto cast_p = IsOp<icraft::xir::Cast>();
	auto prune_axis_p = IsOp<icraft::xir::PruneAxis>(cast_p[0]).setConstraint([](const icraft::xir::Operation &op)
																			  {
		auto prune_axis = op.cast<icraft::xir::PruneAxis>();
		PATTERN_REQUIRE(prune_axis.consumers().size() == 1);
		PATTERN_REQUIRE(prune_axis.consumers()[0]->isInstance<OutputNode>());
		return true; });

	network.rewrite(prune_axis_p, [&](icraft::xrt::Network &network, const icraft::xir::MatchGroup &result)
					{
						auto cast = result.at(cast_p);
						auto prune_axis = result.at(prune_axis_p);
						auto output = prune_axis.consumers()[0];
						auto hardop = cast.producers()[0];

						// 匹配到的是第index个输出
						auto index = output.getInputIndex(prune_axis[0]);
						auto it = std::find(idx_list.begin(), idx_list.end(), *(index.begin()));

						// 可指定分支，去除cast&Pruneaxis；若不输入指定分支，默认去除所有分支的cast&Pruneaxis
						if (it != idx_list.end() || idx_list.size() == 0)
						{
							// 重新连接hardop<->output
							output.setInput(*(index.begin()), hardop[0]);
							// 删除Cast&PruneAxis
							network.removeOpById(prune_axis->op_id);
							network.removeOpById(cast->op_id);
						}
						// 如果不是指定分支，不做任何操作
						else
						{
							network.rewriter().Continue();
						} });
}
// 删除输入分支上的指定pattern（Alignaxis-cast）, 并按照原来input算子的ofm顺序重新连接hardop<->input；
// idx_list用于指定分支删除Alignaxis&cast算子，例如：指定第1条分支删除Alignaxis&cast算子：idx_list={0}
void removeInputCast(icraft::xir::Network &network, bool mmu, icraft::xir::Array<int> idx_list = {})
{
	auto codegen_speedmode = Downcast<icraft::xir::Bool>(network.getTag("speedmode").value())->value;
	auto codegen_compressFtmp = Downcast<icraft::xir::Bool>(network.getTag("compressFtmp").value())->value;
	bool codegen_mmu = codegen_speedmode || codegen_compressFtmp;
	if (codegen_mmu || mmu)
		ICRAFT_LOG(WARNING).append("Open MMU will lock the order of ftmp's physical address, and this may affect network connection!");

	auto input_p = IsOp<icraft::xir::Input>();
	auto align_axis_p = IsOp<icraft::xir::AlignAxis>(input_p);
	auto cast_p = IsOp<icraft::xir::Cast>(align_axis_p[0]);

	network.rewrite(cast_p, [&](icraft::xrt::Network &network, const icraft::xir::MatchGroup &result)
					{
						auto input = result.at(input_p);
						auto align_axis = result.at(align_axis_p);
						auto cast = result.at(cast_p);

						// 提前记录下来cast要连接到地方
						auto cast_uses_info = network.getUsesInfoExceptMatch(cast[0], result);

						// 匹配到的是第index个输出
						auto index = align_axis->inputs[0].index();
						auto it = std::find(idx_list.begin(), idx_list.end(), index);

						// 可指定分支，去除cast&Alignaxis；若不输入指定分支，默认去除所有分支的cast&Alignaxis
						if (it != idx_list.end() || idx_list.size() == 0)
						{
							// 拷贝一份cast的输入，重置一下v_id，防止重名
							auto new_value = cast[0].clone(-1).setId(-1);
							// 重新连接hardop<->input
							input.setOutput(index, new_value);
							// 删除AlignAxis&Cast
							network.removeOpById(align_axis->op_id);
							network.removeOpById(cast->op_id);

							// Input的第index个输入连接到原来cast要连接到地方
							network.connect(input[index], cast_uses_info);
						}
						// 如果不是指定分支，不做任何操作
						else
						{
							network.rewriter().Continue();
						} });
}
std::vector<float> getOutputNormratio(icraft::xir::NetworkView network)
{
	auto network_outp = network.outputs();
	std::vector<float> ret;
	ret.reserve(network_outp.size());
	for (auto &&value : network_outp)
	{
		try
		{
			auto b = value->dtype.getNormratio().value();
			ret.emplace_back(b[0]);
		}
		catch (const std::exception &e)
		{
			std::cout << "the output of network/networkview have no Normratio" << std::endl;
		}
	}
	return ret;
}

std::vector<float> getInputNormratio(icraft::xir::NetworkView network)
{
	auto network_inp = network.inputs();
	std::vector<float> ret;
	ret.reserve(network_inp.size());
	for (auto &&value : network_inp)
	{
		try
		{
			auto b = value->dtype.getNormratio().value();
			ret.emplace_back(b[0]);
		}
		catch (const std::exception &e)
		{
			std::cout << "the input of network/networkview have no Normratio" << std::endl;
		}
	}
	return ret;
}