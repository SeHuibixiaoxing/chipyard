import os
from HybridMapper.CustomYamlDumper import *

OUTPUT_DIR = "output/pipeline/"

class SchedulePointRecord:
    class KEY:
        target = "target"
        model = "model"
        num = "num"
        acc = "acc_per_layer"
        
        point = "point_end_layer"
        
        cost = "cost"
    
    def __init__(self, model_name: str, point_end_layer: list[int], acc_per_layer: int, exec_time: list[int]):
        self.node = dict()
        self.target = dict()
        self.target[self.KEY.model] = model_name
        self.target[self.KEY.acc] = acc_per_layer
        self.target[self.KEY.num] = len(point_end_layer)
        
        self.node[self.KEY.cost] = exec_time
        self.node[self.KEY.point] = point_end_layer
        
        self.node[self.KEY.target] = self.target
        
class SchedulePointRecodList:
    def __init__(self):
        self.record_list: list[SchedulePointRecord] = []
    
    def add_record(self, record: SchedulePointRecord):
        self.record_list.append(record)
    
    def write(self, path: str):
        path = os.path.join(OUTPUT_DIR, "pipeline_schedule_points.yaml")
        assert os.path.exists(os.path.dirname(os.path.normpath(path)))
        node = []
        for record in self.record_list:
            node.append(record.node)
        with open(path, "w") as file:
            yaml.dump(node, file, Dumper=CustomYamlDumper)    
    