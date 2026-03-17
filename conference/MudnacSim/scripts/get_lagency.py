import os
import re
import glob
import csv
import argparse
from collections import defaultdict

BINARY_PATH = "./build/expr_single_pipeline"
OUTPUT_ROOT = "expr/output/expr_single_pipeline/script_output/"
base_dir = "expr/output/expr_single_pipeline/script_output/mudnacsim"

modelList = [
    "resnet18", 
    "bert_ffn",
    "resnet50",
]

layerListDict = {
    "resnet18": [[1,2]],
    "bert_ffn": [[0,1]],
    "resnet50": [[13,14,15,16,17,18]],
}

accList = {
    "resnet18": [8],
    "bert_ffn": [8],
    "resnet50": [16],
}

spmBankList = [2]
lazyFetchList = [0, 1]
doubleBufferList = [0, 1]
pipeSpmStrategyList = ["free", "bankaware"]
dramChannels = [1, 2, 4, 8, 16]
noc_list = ["none", "HomoTileMesh"]
noc_flit_sizes = [8, 16, 32, 64, 128]

# 存储所有结果的数据结构
results = defaultdict(dict)

def run(system, modelName, acc_num, spm_bank_size, layer_list, doubleBuffer, lazyFetch, pipeSpmStrategy, noc, channels, noc_flit_size):
    layer_list_str = ""
    for layer_id in layer_list:
        layer_list_str += f"-{layer_id}"
        
    doubleBufferStr = "_doublebuffer" if doubleBuffer == 1 else ""
    lazyFetchStr = "_lazyfetch" if lazyFetch == 1 else ""
    prefix_name = f"{modelName}{layer_list_str}_{system}_dram-{channels}_noc-{noc}-{noc_flit_size}_{acc_num}acc_{spm_bank_size*acc_num}MB_{pipeSpmStrategy}{doubleBufferStr}{lazyFetchStr}"
    
    # 查找匹配的文件
    file_pattern = os.path.join(base_dir, f"{prefix_name}_batch*")
    files = glob.glob(file_pattern)
    
    if not files:
        return
    
    # 处理每个文件
    for file_path in files:
        try:
            # 从文件名中提取批处理量
            filename = os.path.basename(file_path)
            batch_match = re.search(r'batch(\d+)$', filename)
            if not batch_match:
                print(f"警告: 无法从文件名 {filename} 中提取批处理量")
                continue
                
            batch_num = batch_match.group(1)
            
            # 从文件中提取延迟时间
            latency = None
            with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
                for line in f:
                    if "Total Latency: " in line:
                        latency_match = re.search(r'Total Latency:\s+(\d+\.?\d*)', line)
                        if latency_match:
                            latency = float(latency_match.group(1))
                            break
            
            if latency:
                # 存储结果
                hd_key = (channels, noc_flit_size)
                config_key = (doubleBuffer, lazyFetch)
                
                if modelName not in results:
                    results[modelName] = {}
                
                if hd_key not in results[modelName]:
                    results[modelName][hd_key] = {}
                
                if config_key not in results[modelName][hd_key]:
                    results[modelName][hd_key][config_key] = {}
                
                results[modelName][hd_key][config_key][batch_num] = latency
                
            else:
                print(f"warn: 在文件 {filename} 中未找到延迟信息")
                
        except Exception as e:
            print(f"处理文件 {file_path} 时出错: {str(e)}")

def calculate_speedup():
    """计算并输出加速比结果"""
    # 创建CSV文件存储结果
    with open('speedup_results.csv', 'w', newline='') as csvfile:
        fieldnames = [
            'DRAM_Channels', 'NoC_Flit_Size', 'Model', 
            'LazyFetch', 'DoubleBuffer', 'Batch_Size', 'Latency', 
            'Speedup'
        ]
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        
        # 创建加速比字典
        speedup_dict = defaultdict(lambda: defaultdict(dict))
        
        # 遍历所有模型
        for model, model_data in results.items():
            # 遍历所有硬件配置
            for hd_key, hd_data in model_data.items():
                channels, noc_flit_size = hd_key
                
                # 创建该硬件配置的加速比字典
                config_speedup = defaultdict(list)
                
                # 获取所有批处理量
                all_batches = set()
                for config_data in hd_data.values():
                    all_batches.update(config_data.keys())
                sorted_batches = sorted(all_batches, key=int)
                
                # 遍历所有批处理量
                for batch_size in sorted_batches:
                    # 获取基准配置 (doubleBuffer=0, lazyFetch=0)
                    baseline_key = (0, 0)
                    baseline_latency = None
                    
                    # 查找基准配置的延迟
                    if baseline_key in hd_data and batch_size in hd_data[baseline_key]:
                        baseline_latency = hd_data[baseline_key][batch_size]
                    
                    # 如果没有基准配置数据，跳过该批处理量
                    if baseline_latency is None:
                        continue
                    
                    # 计算所有配置相对于基准的加速比
                    for config_key, batch_data in hd_data.items():
                        doubleBuffer, lazyFetch = config_key
                        
                        # 跳过基准配置自身
                        if config_key == baseline_key:
                            continue
                        
                        # 获取当前配置的延迟
                        if batch_size in batch_data:
                            latency = batch_data[batch_size]
                            
                            # 计算加速比
                            speedup = baseline_latency / latency
                            
                            # 添加到加速比字典
                            config_speedup[config_key].append(speedup)
                            
                            # 写入CSV
                            writer.writerow({
                                'Model': model,
                                'DRAM_Channels': channels,
                                'NoC_Flit_Size': noc_flit_size,
                                'Batch_Size': batch_size,
                                'DoubleBuffer': doubleBuffer,
                                'LazyFetch': lazyFetch,
                                'Latency': latency,
                                'Speedup': speedup
                            })
                
                # 将加速比字典添加到主字典
                speedup_dict[model][hd_key] = config_speedup
    
    print("加速比结果已保存到 speedup_results.csv")
    
    # 输出加速比字典
    print("\n加速比字典:")
    for model, model_data in speedup_dict.items():
        print(f"\n模型: {model}")
        for hd_key, hd_data in model_data.items():
            channels, noc_flit_size = hd_key
            print(f"  硬件配置: DRAM通道={channels}, NoC Flit大小={noc_flit_size}")
            
            for config_key, speedup_list in hd_data.items():
                doubleBuffer, lazyFetch = config_key
                print(f"    配置: DoubleBuffer={doubleBuffer}, LazyFetch={lazyFetch}")
                print(f"      加速比列表: {speedup_list}")
    
    return speedup_dict

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--num-proc", type=int, default=5)
    
    args = parser.parse_args()

    system = "spm"
    numProcesses = args.num_proc

    # 遍历所有配置
    for noc in noc_list:
        for dram_channel in dramChannels:
            for noc_flit_size in noc_flit_sizes:
                for modelName in modelList:
                    for acc_num in accList[modelName]:
                        for spm_bank_size in spmBankList:
                            for layer_list in layerListDict[modelName]:
                                for pipeStrategy in pipeSpmStrategyList:
                                    for doubleBuffer in doubleBufferList:
                                        for lazyFetch in lazyFetchList:
                                            run(system, modelName, acc_num, spm_bank_size, layer_list, doubleBuffer, lazyFetch, pipeStrategy, noc, dram_channel, noc_flit_size)
    
    # 计算并输出加速比
    calculate_speedup()

if __name__ == "__main__":
    main()