from HybridMapper.Target import Target

def get_graph_partition_acc_candidate():
    return [
        # 8,
        # 12,
        16,
        # 20,
        # 24,
        # 28,
        32
    ]

def get_layer_mapping_acc_candidate():
    return [
        1,
        2,
        4,
        8,
        16,
        32
    ]

def get_spm_kb_per_core_candidate():
    return [
        0.5 * 1024,
        1 * 1024,
        2 * 1024,
        4 * 1024,
        # 8 * 1024
    ]
def get_dram_bw_per_cycle_candidate():
    base_bw = 19
    return [base_bw * factor for factor in [
        1, 
        # 2, 
        # 3,
        # 4
    ]]

def get_noc_bw_per_cycle_candidate():
    return [
        32,
        64
    ]

def get_mac_per_acc_candidate():
    return [
        32 * 32
    ]

def get_default_batch_candidate():
    return [
        1,
        2,
        4,
        8,
        16,
        32
    ]

def get_target_set():
    return [
        Target(1, 32 * 2 * 1024, enablePipeline=True),
        Target(2, 32 * 2 * 1024, enablePipeline=True),
        Target(4, 32 * 2 * 1024, enablePipeline=True),
        Target(8, 32 * 2 * 1024, enablePipeline=True),
        Target(16, 32 * 2 * 1024, enablePipeline=True),
        Target(32, 32 * 2 * 1024, enablePipeline=True),
        Target(64, 32 * 2 * 1024, enablePipeline=True),
    ]

def get_mapping_target():
    re = []
    for acc in get_layer_mapping_acc_candidate():
        re.append(Target(acc, acc * 128 * 1024, enablePipeline=True))
    
    return re
def get_graph_target():
    re = []
    for acc in get_graph_partition_acc_candidate():
        for dram_bw_per_cycle in get_dram_bw_per_cycle_candidate():
            for noc_bw_per_cycle in get_noc_bw_per_cycle_candidate():
                re.append(Target(acc, acc * 8 * 1024, enablePipeline=True, dram_bw_per_cycle=dram_bw_per_cycle, noc_bw_per_cycle=noc_bw_per_cycle))
    return re

def get_model_name_list():
    return [
        "resnet50", # ok
        "mobilenetv2", # ok
        "effinetb0", # ok
        "vitb16", # ok
        "pointpillars", # ok
        "w2v2base", # ok
        "bertbase", # ok
        "gnmt", # dookne
        
        "berttiny",
        "bertmini",
        "bertsmall",
        "bertmedium",

        # "ptv3",
        # "onet",
        # "resnet18", # ok
        # "testmodel",
        # "unet",
        
        # "bert_mha",
        # "bert_ffn"
    ]