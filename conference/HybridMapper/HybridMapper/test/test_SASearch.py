import os
import pickle
from HybridMapper.SASearch import PipelineSolution,PipelineTarget
from HybridMapper.StageRecord import TensorType
from HybridMapper.Model import Model
from HybridMapper.Target import Target

def load_model_and_mapping(model_name="resnet18"):
    """Load model and mapping data"""
    base_dir = f"output/pipeline/{model_name}"
    model_path = os.path.join(base_dir, "layers.dump")
    mapping_path = os.path.join(base_dir, "mapping")
    
    # Check if required files exist
    if not os.path.exists(model_path):
        raise FileNotFoundError(f"Model file not found: {model_path}")
    if not os.path.exists(mapping_path):
        raise FileNotFoundError(f"Mapping directory not found: {mapping_path}")
    
    # Load model
    with open(model_path, 'rb') as f:
        model = pickle.load(f)
        
    # Load layer mapping data
    model.load_layer_mapping(mapping_path)
    
    return model, mapping_path

def create_test_segment_solution(model: Model, mapping_path: str):
    """Create a SegmentSolution with specific configurations"""
    # Create pipeline target (to be configured by user)
    target = Target(22, 22 * 2 * 1024, enablePipeline=True, dram_bw_per_cycle=12, noc_bw_per_cycle=64)
    pipeline_target = PipelineTarget(
        model=model,
        layer_mapping_dir_path=mapping_path,
        start_layer_idx=0,
        end_layer_idx=5,
        default_batch=32,
        target=target
    )
    
    # Define tensor types for each layer (to be configured by user)
    tensor_type_dict_list = [
        {0: TensorType.IO_DOUBLE, 1: TensorType.INTER_SINGLE}, #0
        {1: TensorType.INTER_DOUBLE, 2: TensorType.INTER_DRAM_SINGLE}, #1
        {2: TensorType.INTER_DRAM_DOUBLE, 3: TensorType.INTER_PURE_DECOUPLING}, #2
        {3: TensorType.INTER_PURE_DECOUPLING, 4: TensorType.INTER_SHARED_READ, 1: TensorType.INTER_SINGLE}, #3
        {4: TensorType.INTER_SHARED_READ, 5: TensorType.INTER_SHARED_WRITE}, #4
        {5: TensorType.INTER_SHARED_WRITE, 6: TensorType.IO_SINGLE} #5
    ]
    
    # Define accelerator allocation (to be configured by user)
    num_acc_list = [4, 4, 4, 2, 4, 4]
    
    # Create SegmentSolution
    segment_solution = PipelineSolution.SegmentSolution(
        start_layer_idx=0,
        end_layer_idx=5,
        model=model,
        tensor_type_dict_list=tensor_type_dict_list,
        num_acc_list=num_acc_list,
        pipeline_target=pipeline_target
    )
    
    return segment_solution, pipeline_target

def print_segment_solution_details(segment_solution: PipelineSolution.SegmentSolution, pipeline_target: PipelineTarget):
    """Print key results of the segment solution for manual inspection"""
    print("=== SegmentSolution Test Results ===")
    print(f"Number of layers: {len(segment_solution._layer_list)}")
    print(f"Total DRAM bytes: {segment_solution._total_dram_bytes}")
    print(f"Stage cycles: {segment_solution._stage_cycles}")
    print(f"Segment evaluation: {segment_solution.evaluate()}")
    print()
    
    # Print tensor type configurations
    print("Tensor Type Configurations:")
    for i, (layer, tensor_type_dict) in enumerate(zip(segment_solution._layer_list, segment_solution._tensor_type_dict_list)):
        print(f"  Layer {i} ({layer.type}):")
        input_ids = layer.getInputTensorId()
        output_ids = layer.getOutputTensorId()
        weight_ids = layer.getWeightTensorId()
        
        for tensor_id in input_ids:
            tensor_type = tensor_type_dict.get(tensor_id, TensorType.UNKNOWN)
            print(f"    Input Tensor {tensor_id}: {tensor_type.name}, meta type: {segment_solution._tensor_meta_type[tensor_id]}")
            
        for tensor_id in output_ids:
            tensor_type = tensor_type_dict.get(tensor_id, TensorType.UNKNOWN)
            print(f"    Output Tensor {tensor_id}: {tensor_type.name}, meta type: {segment_solution._tensor_meta_type[tensor_id]}")
    
    print()
    
    # Print layer solution details
    print("Layer Solution Details:")
    for i, layer_solution in enumerate(segment_solution._layer_solution_list):
        print(f"  Layer {i}:")
        print(f"    Compute cycles: {layer_solution.get_compute_cycles()}")
        print(f"    DRAM bytes: {layer_solution.get_dram_bytes()}")
        print(f"    DRAM bypass: {layer_solution._dram_bypass}")
        print(f"    SPM bypass: {layer_solution._spm_bypass}")
        print(f"    Layer mapping exists: {layer_solution._layer_mapping is not None}")
    print()
    
    # Print hardware target info
    print("Hardware Target Info:")
    print(f"  Number of accelerators: {pipeline_target.target.numArrays}")
    print(f"  SPM size: {pipeline_target.target.spmBytes // 1024 / 1024} MB")
    print(f"  DRAM bandwidth per cycle: {pipeline_target.target.dram_bw_per_cycle} bytes/cycle")
    print(f"  NoC bandwidth per cycle: {pipeline_target.target.noc_bw_per_cycle} bytes/cycle")
    print(f"  Default batch size: {pipeline_target.default_batch}")
    print()
    
    # Print NoC cycles
    print("NoC Cycles per Layer:")
    for i, noc_cycles in enumerate(segment_solution._noc_cycles):
        print(f"  Layer {i}: {noc_cycles}")
    print()
    
    # Print tensor metadata
    print("Tensor Metadata:")
    print(f"  Tensor meta types: {segment_solution._tensor_meta_type}")
    print(f"  Tensor in degree: {segment_solution._tensor_in_degree}")
    print(f"  Tensor out degree: {segment_solution._tensor_out_degree}")
    print(f"  Tensor max size: {segment_solution._tensor_max_size}")
    print(f"  Tensor min size: {segment_solution._tensor_min_size}")
    print(f"  Tensor Ids : {segment_solution._tensor_ids}")
    print()
    
    # Print stage info
    print("Stage Info:")
    print(f"  Depth list: {segment_solution._depth_list}")
    print(f"  Tensor to input stage mapping: {segment_solution._tensor_id_to_as_input_stage_idx_vec}")
    print(f"  Tensor to output stage mapping: {segment_solution._tensor_id_to_as_output_stage_idx_vec}")
    print()
    
    # Print ring buffer info
    print("Ring Buffer Info:")
    print(f"  Ring buffer count: {segment_solution._ring_buffer_count}")
    print(f"  Ring buffer size per tensor: {segment_solution._ring_buffer_size_per}")
    print(f"  Ring buffer use count: {segment_solution._ring_buffer_use_count}")
    print(f"  Ring buffer use spm: {segment_solution._ring_buffer_use_spm}")
    print()
    
    # Print SPM usage
    print("SPM Usage:")
    print("  SPM util in stage:")
    for stage_idx, stage_util_dict in enumerate(segment_solution._tensor_spm_util_in_stage):
        if stage_util_dict:  # 只打印非空字典
            print(f"    Stage {stage_idx}: {stage_util_dict}")    
    print(f"  SPM util shared: {segment_solution._tensor_spm_util_shared}")
    print(f"  SPM util in ringbuffer: {segment_solution._tensor_spm_util_in_ringbuffer}")
    print(f"  SPM util in weigth tensor: {segment_solution._total_weight_spm_util}")
    print(f"  SPM util in io tensor: {segment_solution._total_io_spm_util}")
    print(f"  Total SPM util: {segment_solution._total_spm_util}")
    print()
    
    # Print DRAM access details
    print("DRAM Access Details:")
    for i, layer_solution in enumerate(segment_solution._layer_solution_list):
        print(f"  Layer {i}:")
        dram_tensor_dict = layer_solution.get_dram_bytes_tensor_dict()
        print(f"    DRAM bytes per tensor: {dram_tensor_dict}")
    print()
    
    # Print cycle component breakdown
    print("Cycle Component Breakdown:")
    for i in range(len(segment_solution._layer_list)):
        stage_cycles = segment_solution._stage_cycles[i]
        compute_cycles = segment_solution._layer_solution_list[i].get_compute_cycles()
        dram_cycles = segment_solution._dram_cycles[i]
        noc_cycles_dict = segment_solution._noc_cycles[i]
        noc_cycles_total = sum(noc_cycles_dict.values()) if noc_cycles_dict else 0
        
        print(f"  Stage {i}:")
        print(f"    Total cycles: {stage_cycles}")
        print(f"    Compute cycles: {compute_cycles}")
        print(f"    DRAM cycles: {dram_cycles}")
        print(f"    NoC cycles: {noc_cycles_dict} (Total: {noc_cycles_total})")
    print()
    
    # Print model layer information
    print("Model Layer Information:")
    for i, layer in enumerate(segment_solution._layer_list):
        print(f"  Layer {i} ({layer.type}):")
        print(f"    Parameters: {layer.param}")
        print(f"    Data IDs: {layer.data}")
        print(f"    Data sizes: {layer.size}")
        print(f"    Compute: {layer.compute}")
        print(f"    Input tensor IDs: {layer.getInputTensorId()}")
        print(f"    Output tensor IDs: {layer.getOutputTensorId()}")
        print(f"    Weight tensor IDs: {layer.getWeightTensorId()}")
        print(f"    Total data size: {layer.getTotalDataSize()}")
        print(f"    Compute intensity: {layer.getComputeIntensity()}")
    print()

def main():
    try:
        # Load model and mapping
        model, mapping_path = load_model_and_mapping("resnet18")
        print("Successfully loaded ResNet18 model and mapping data")
        print()
        
        # Create test segment solution
        segment_solution, pipeline_target = create_test_segment_solution(model, mapping_path)
        
        # Print results
        print_segment_solution_details(segment_solution, pipeline_target)
                
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()

if __name__ == "__main__":
    main()