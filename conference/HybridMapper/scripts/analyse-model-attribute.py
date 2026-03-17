import os
import argparse
import pickle
import numpy as np
import matplotlib.pyplot as plt
from HybridMapper.GraphPartition import GraphPartitioner
from HybridMapper.Target import Target
from HybridMapper.PipelineTarget import get_graph_target,get_default_batch_candidate,get_model_name_list
from HybridMapper.Model import Model,Layer

OUTPUT_DIR = "output/pipeline/"
def load_model(model_path):
    """
    Load a model from a pickle dump file.
    
    Args:
        model_path (str): Path to the model dump file
        
    Returns:
        Model: Loaded model object
    """
    with open(model_path, 'rb') as f:
        model = pickle.load(f)
    return model

def generate_plots(model: Model, layer_indices, compute_intensity_list, normalized_intensity, 
                   total_data_size_kb, weight_data_size_kb, io_data_size_kb, model_name):
    """
    Generate plots for model analysis.
    
    Args:
        model: Model object
        layer_indices: List of layer indices
        compute_intensity_list: List of compute/data size ratios
        normalized_intensity: List of normalized ratios
        total_data_size_kb: List of total data size in KB
        weight_data_size_kb: List of weight data size in KB
        io_data_size_kb: List of IO data size in KB
        model_name: Name of the model
    """
    # 创建图表
    fig, axes = plt.subplots(5, 1, figsize=(12, 16))
    
    # 第一个子图：原始比值
    axes[0].plot(layer_indices, compute_intensity_list, marker='o', linestyle='-', color='blue')
    axes[0].set_title(f'{model_name} - Compute/Data Size Ratio per Layer')
    axes[0].set_xlabel('Layer Index')
    axes[0].set_ylabel('Compute/Data Size Ratio')
    axes[0].grid(True, alpha=0.3)
    
    # 第二个子图：归一化比值
    axes[1].plot(layer_indices, normalized_intensity, marker='s', linestyle='-', color='red')
    axes[1].set_title(f'{model_name} - Normalized Compute/Data Size Ratio per Layer')
    axes[1].set_xlabel('Layer Index')
    axes[1].set_ylabel('Normalized Ratio')
    axes[1].grid(True, alpha=0.3)
    
    # 第三个子图：直方图
    axes[2].hist(compute_intensity_list, bins=20, color='green', alpha=0.7, edgecolor='black')
    axes[2].set_title(f'{model_name} - Distribution of Compute/Data Size Ratios')
    axes[2].set_xlabel('Compute/Data Size Ratio')
    axes[2].set_ylabel('Frequency')
    axes[2].grid(True, alpha=0.3)
    
    # 第四个子图：内存使用情况
    axes[3].plot(layer_indices, total_data_size_kb, marker='^', linestyle='-', color='orange', label='Total')
    axes[3].plot(layer_indices, weight_data_size_kb, marker='o', linestyle='-', color='blue', label='Weight')
    axes[3].plot(layer_indices, io_data_size_kb, marker='s', linestyle='-', color='green', label='IO')
    axes[3].set_title(f'{model_name} - Data Size per Layer')
    axes[3].set_xlabel('Layer Index')
    axes[3].set_ylabel('Data Size (KB)')
    axes[3].legend()
    axes[3].grid(True, alpha=0.3)
    
    # 第五个子图：权重和IO数据的堆叠图
    axes[4].bar(layer_indices, weight_data_size_kb, label='Weight', color='blue')
    axes[4].bar(layer_indices, io_data_size_kb, bottom=weight_data_size_kb, label='IO', color='green')
    axes[4].set_title(f'{model_name} - Weight and IO Data Size Distribution')
    axes[4].set_xlabel('Layer Index')
    axes[4].set_ylabel('Data Size (KB)')
    axes[4].legend()
    axes[4].grid(True, alpha=0.3)
    
    # 调整布局
    plt.tight_layout()
    
    # 保存图表
    output_dir = os.path.join(OUTPUT_DIR, model_name)
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
        
    plot_path = os.path.join(output_dir, "compute_data_ratio_analysis.png")
    plt.savefig(plot_path, dpi=300, bbox_inches='tight')
    plt.close()
    
    print(f"\nAnalysis plot saved to: {plot_path}")
    
    # 生成单独的CSV文件用于进一步分析
    csv_path = os.path.join(output_dir, "compute_data_ratio_analysis.csv")
    with open(csv_path, 'w') as f:
        f.write("Layer_Index,Compute,Data_Size,Weight_Size,IO_Size,Ratio,Normalized_Ratio,Total_Data_Size_KB,Weight_Size_KB,IO_Size_KB\n")
        for i, (layer_idx, original_ratio, normalized_ratio, total_kb, weight_kb, io_kb) in enumerate(
            zip(layer_indices, compute_intensity_list, normalized_intensity, total_data_size_kb, weight_data_size_kb, io_data_size_kb)
        ):
            f.write(f"{i},{model.layerList[layer_idx].compute},{model.layerList[layer_idx].getTotalDataSize()},{model.layerList[layer_idx].getTotalSize(model.get_segment_layer_weight_ids(0, model.getNumLayers() - 1)[layer_idx])},{model.layerList[layer_idx].getTotalSize(model.get_segment_layer_input_ids(0, model.getNumLayers() - 1)[layer_idx] + model.get_segment_layer_output_ids(0, model.getNumLayers() - 1)[layer_idx])},{original_ratio:.6f},{normalized_ratio:.6f},{total_kb:.2f},{weight_kb:.2f},{io_kb:.2f}\n")
    
    print(f"Analysis data saved to: {csv_path}")

def compare_models(models_data):
    """
    Compare multiple models on compute/data size ratio, weight size, IO size, and total data size.
    
    Args:
        models_data (dict): Dictionary with model names as keys and (model, metrics) tuples as values
    """
    model_names = list(models_data.keys())
    
    # Extract metrics for all models
    avg_ratios = []
    avg_weight_sizes = []   # Changed from total to average
    avg_io_sizes = []       # Changed from total to average
    avg_data_sizes = []     # Changed from total to average
    
    for model_name, (model, metrics) in models_data.items():
        # Calculate average compute/data size ratio
        avg_ratio = np.mean(metrics['compute_intensity_list'])
        avg_ratios.append(avg_ratio)
        
        # Calculate average sizes instead of total sizes
        avg_weight_size = np.mean(metrics['weight_data_size_kb'])   # Average instead of sum
        avg_io_size = np.mean(metrics['io_data_size_kb'])           # Average instead of sum
        avg_data_size = np.mean(metrics['total_data_size_kb'])      # Average instead of sum
        
        avg_weight_sizes.append(avg_weight_size)
        avg_io_sizes.append(avg_io_size)
        avg_data_sizes.append(avg_data_size)
    
    # Create comparative plots
    fig, axes = plt.subplots(2, 2, figsize=(15, 12))
    fig.suptitle('Model Comparison Analysis', fontsize=16)
    
    # 1. Average Compute/Data Size Ratio Comparison
    bars1 = axes[0, 0].bar(model_names, avg_ratios, color='skyblue')
    axes[0, 0].set_title('Average Compute/Data Size Ratio Comparison')
    axes[0, 0].set_ylabel('Average Ratio')
    axes[0, 0].tick_params(axis='x', rotation=45)
    # Add value labels on bars
    for bar, value in zip(bars1, avg_ratios):
        axes[0, 0].text(bar.get_x() + bar.get_width()/2, bar.get_height(), 
                       f'{value:.2f}', ha='center', va='bottom')
    
    # 2. Average Weight Size Comparison (changed from total)
    bars2 = axes[0, 1].bar(model_names, avg_weight_sizes, color='lightcoral')
    axes[0, 1].set_title('Average Weight Size per Layer Comparison')
    axes[0, 1].set_ylabel('Average Weight Size (KB)')
    axes[0, 1].tick_params(axis='x', rotation=45)
    # Add value labels on bars
    for bar, value in zip(bars2, avg_weight_sizes):
        axes[0, 1].text(bar.get_x() + bar.get_width()/2, bar.get_height(), 
                       f'{value:.2f}', ha='center', va='bottom')
    
    # 3. Average IO Size Comparison (changed from total)
    bars3 = axes[1, 0].bar(model_names, avg_io_sizes, color='lightgreen')
    axes[1, 0].set_title('Average IO Size per Layer Comparison')
    axes[1, 0].set_ylabel('Average IO Size (KB)')
    axes[1, 0].tick_params(axis='x', rotation=45)
    # Add value labels on bars
    for bar, value in zip(bars3, avg_io_sizes):
        axes[1, 0].text(bar.get_x() + bar.get_width()/2, bar.get_height(), 
                       f'{value:.2f}', ha='center', va='bottom')
    
    # 4. Average Data Size Comparison (changed from total)
    bars4 = axes[1, 1].bar(model_names, avg_data_sizes, color='gold')
    axes[1, 1].set_title('Average Data Size per Layer Comparison')
    axes[1, 1].set_ylabel('Average Data Size (KB)')
    axes[1, 1].tick_params(axis='x', rotation=45)
    # Add value labels on bars
    for bar, value in zip(bars4, avg_data_sizes):
        axes[1, 1].text(bar.get_x() + bar.get_width()/2, bar.get_height(), 
                       f'{value:.2f}', ha='center', va='bottom')
    
    plt.tight_layout()
    
    # Save comparative plot
    output_dir = os.path.join(OUTPUT_DIR, "comparison")
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
        
    plot_path = os.path.join(output_dir, "model_comparison.png")
    plt.savefig(plot_path, dpi=300, bbox_inches='tight')
    plt.close()
    
    print(f"\nComparison plot saved to: {plot_path}")
    
    # Generate comparative CSV
    csv_path = os.path.join(output_dir, "model_comparison.csv")
    with open(csv_path, 'w') as f:
        f.write("Model, Avg_Compute_Data_Ratio, Avg_Weight_Size_KB, Avg_IO_Size_KB, Avg_Data_Size_KB\n")
        for i, model_name in enumerate(model_names):
            f.write(f"{model_name}, {avg_ratios[i]:.6f}, {avg_weight_sizes[i]:.2f}, "
                   f"{avg_io_sizes[i]:.2f}, {avg_data_sizes[i]:.2f}\n")
    
    print(f"Comparison data saved to: {csv_path}")
    
    # Print comparison summary
    print("\nModel Comparison Summary:")
    print("-" * 95)
    print(f"{'Model':<20} {'Avg Ratio':<15} {'Avg Weight(KB)':<20} {'Avg IO(KB)':<15} {'Avg Data(KB)':<15}")
    print("-" * 95)
    for i, model_name in enumerate(model_names):
        print(f"{model_name:<20} {avg_ratios[i]:<15.4f} {avg_weight_sizes[i]:<20.2f} "
              f"{avg_io_sizes[i]:<15.2f} {avg_data_sizes[i]:<15.2f}")

def analyze_model_attributes(model: Model, model_name: str, return_metrics=False):
    """
    Analyze model layer attributes and generate statistics.
    
    Args:
        model (Model): Model object to analyze
        model_name (str): Name of the model
        return_metrics (bool): Whether to return metrics for comparison
        
    Returns:
        dict: Metrics dictionary if return_metrics is True, else None
    """
    # 计算每层的compute/data size比值和内存需求
    compute_intensity_list = []
    total_data_size_list = []
    weight_data_size_list = []
    io_data_size_list = []
    layer_indices = []
    
    for i, layer in enumerate(model.layerList):
        total_data_size =  layer.getTotalDataSize()
        weight_data_size = layer.getTotalSize(model.get_segment_layer_weight_ids(0, model.getNumLayers() - 1)[i])
        io_data_size = layer.getTotalSize(model.get_segment_layer_input_ids(0, model.getNumLayers() - 1)[i] + model.get_segment_layer_output_ids(0, model.getNumLayers() - 1)[i])
        compute = layer.compute
        
        # 避免除以零
        if total_data_size > 0:
            compute_intensity = compute / total_data_size
        else:
            compute_intensity = 0
            
        compute_intensity_list.append(compute_intensity)
        total_data_size_list.append(total_data_size)
        weight_data_size_list.append(weight_data_size)
        io_data_size_list.append(io_data_size)
        layer_indices.append(i)
    
    compute_intensity_array = np.array(compute_intensity_list)
    total_data_size_list_array = np.array(total_data_size_list)
    weight_data_size_array = np.array(weight_data_size_list)
    io_data_size_array = np.array(io_data_size_list)
    
    # 转换内存使用量为KB
    total_data_size_kb = total_data_size_list_array / 1024.0
    weight_data_size_kb = weight_data_size_array / 1024.0
    io_data_size_kb = io_data_size_array / 1024.0
    
    # 归一化处理,避免所有值都为0的情况
    if np.max(compute_intensity_array) > 0:
        normalized_intensity = compute_intensity_array / np.max(compute_intensity_array)
    else:
        normalized_intensity = compute_intensity_array
    
    # 输出结果
    print(f"Model: {model_name}")
    print("Layer Compute/Data Size Ratio and Total Data Size Analysis:")
    print("-" * 110)
    print(f"{'Layer':<8} {'Compute':<15} {'Data Size':<15} {'Weight Size':<15} {'IO Size':<15} {'Ratio':<15} {'Normalized':<15} {'Total Data Size(KB)':<20}")
    print("-" * 110)
    
    for i, (layer, original_ratio, normalized_ratio, total_kb, weight_kb, io_kb) in enumerate(
        zip(model.layerList, compute_intensity_list, normalized_intensity, total_data_size_kb, weight_data_size_kb, io_data_size_kb)
    ):
        print(f"{i:<8} {layer.compute:<15} {layer.getTotalDataSize():<15} {weight_kb:<15} {io_kb:<15} {original_ratio:<15.2f} {normalized_ratio:<15.4f} {total_kb:<20.2f}")
    
    # 生成统计信息
    print("\nStatistics:")
    print("-" * 30)
    print(f"Mean Ratio: {np.mean(compute_intensity_array):.4f}")
    print(f"Std Deviation: {np.std(compute_intensity_array):.4f}")
    print(f"Min Ratio: {np.min(compute_intensity_array):.4f}")
    print(f"Max Ratio: {np.max(compute_intensity_array):.4f}")
    print(f"Median Ratio: {np.median(compute_intensity_array):.4f}")
    
    # 内存使用统计
    print("\nTotal Data Size Statistics:")
    print("-" * 30)
    print(f"Total Data Size (KB): {np.sum(total_data_size_kb):.2f}")
    print(f"Total Weight Size (KB): {np.sum(weight_data_size_kb):.2f}")
    print(f"Total IO Size (KB): {np.sum(io_data_size_kb):.2f}")
    print(f"Mean Memory per Layer (KB): {np.mean(total_data_size_kb):.2f}")
    print(f"Max Memory Layer (KB): {np.max(total_data_size_kb):.2f}")
    print(f"Min Memory Layer (KB): {np.min(total_data_size_kb):.2f}")
    
    # 生成统计图
    generate_plots(model, layer_indices, compute_intensity_list, normalized_intensity, 
                   total_data_size_kb, weight_data_size_kb, io_data_size_kb, model_name)
    
    if return_metrics:
        return {
            'compute_intensity_list': compute_intensity_list,
            'total_data_size_kb': total_data_size_kb,
            'weight_data_size_kb': weight_data_size_kb,
            'io_data_size_kb': io_data_size_kb,
            'normalized_intensity': normalized_intensity,
            'layer_indices': layer_indices
        }
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--models', type=lambda s: s.split(','),  default=[], help='逗号分隔的模型名称')

    
    args = parser.parse_args()
    
    # Handle single model analysis
    model_list = get_model_name_list() if not args.models else args.models
        
    models_data = {}
    for model_name in model_list:
        base_dir = os.path.join(OUTPUT_DIR, model_name)
        model_dump_path = os.path.join(base_dir, "layers.dump")
    
        print(f"Loading model: {model_name} from path: {model_dump_path}")
            
        model: Model = load_model(model_dump_path)
        model.load_layer_mapping(os.path.join(base_dir, "mapping"))
        print(f"Analyzing model {model_name}...")
        metrics = analyze_model_attributes(model, model_name, return_metrics=True)
        models_data[model_name] = (model, metrics)
        
    if len(models_data) > 1:
        print("\nComparing models...")
        compare_models(models_data)

if __name__ == "__main__":
    main()