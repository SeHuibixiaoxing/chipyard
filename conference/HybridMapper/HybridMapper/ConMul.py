import gurobipy as gu


class ConMul:
    def __init__(self, model: gu.Model):
        self.model = model

    def __call__(self, cm: list):
        reduced = list(cm)
        while len(reduced) > 1:
            i = 0
            newReduced = []
            while i < len(reduced):
                if i + 1 < len(reduced):
                    if (isinstance(reduced[i], int) or isinstance(reduced[i], float) or
                            isinstance(reduced[i + 1], int) or isinstance(reduced[i + 1], float)):
                        newReduced.append(reduced[i] * reduced[i + 1])
                    else:
                        auxVar = self.model.addVar(lb=0, vtype=gu.GRB.INTEGER)
                        self.model.addConstr(reduced[i] * reduced[i + 1] == auxVar)
                        newReduced.append(auxVar)
                else:
                    newReduced.append(reduced[i])
                i += 2
            reduced = newReduced
        return reduced[0]

class Ceil:
    def __init__(self, model: gu.Model):
        self.model = model
    
    def __call__(self, x):
        floor_x = self.model.addVar(lb=1, vtype=gu.GRB.INTEGER)

        # 添加约束：x ≤ floor_x < x + 1
        self.model.addConstr(floor_x <= (x + 0.99999))
        self.model.addConstr(x <= floor_x)
        return floor_x