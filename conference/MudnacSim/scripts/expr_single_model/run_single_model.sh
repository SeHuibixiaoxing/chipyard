#!/bin/bash
set -e -x -u

# 提取参数
ext_values="$1"

accIdx=3

# 生成日志名后缀（将逗号替换为下划线）
log_suffix=$(echo "$ext_values" | tr ',' '_')

# model_name="resnet18"
# model_name="bertpipeline_mha"
# model_name="bertpipeline_ffn"
model_name="resnet50"

# 执行命令并输出到日志
mkdir -p logs
log_name="${model_name}_accidx_${accIdx}_spmbanksize_2_ext_${log_suffix}"
log="logs/${log_name}.log"

sh scripts/build.sh -d -j 2

{
    python scripts/expr_single_model2.py \
        --acc-num-idx ${accIdx} \
        --spm-bank-size 2 \
        --ext $ext_values \
        --log-name ${log_name} \
        --model ${model_name}
} 2>&1 | tee $log
