import numpy as np
import onnx
import onnx_graphsurgeon as gs
from collections import OrderedDict

def get_quant_nodes(graph):
    quant_nodes = []
    exclude_nodes = [] # ["MatMul_178", "MatMul_141", "MatMul_119", "MatMul_125", 
                       #  "MatMul_131", "Transpose_173", "Reshape_177"]
    for node in graph.nodes:
        if node.op in ["Conv"]:
            if node.attrs['group']==1:
                quant_nodes.append(node.name)
        if node.op == "MatMul" and \
            isinstance(node.inputs[1], gs.Constant):
            quant_nodes.append(node.name)

    for node in graph.nodes:
        if node.op in ["Softmax", ]:
            print("encoder_quant_exclude_nodes: ", node.name)
            exclude_nodes.append(node.name)
        if node.op == "Add" and \
            "norm" in node.inputs[1].name:
            print("encoder_quant_exclude_nodes: ", node.name, " ", node.inputs[1].name)
            exclude_nodes.append(node.name)
        if node.op == "Mul" and \
            "norm" in node.inputs[1].name:
            print("encoder_quant_exclude_nodes: ", node.name, " ", node.inputs[1].name)
            exclude_nodes.append(node.name)

    with open("encoder_quant_nodes.txt", "w+") as f:
        f.write('\n'.join(quant_nodes))
    with open("encoder_quant_exclude_nodes.txt", "w+") as f:
        f.write('\n'.join(exclude_nodes))

def wenet_encoder():
    encoder = onnx.load("model/encoder.onnx")
    graph =  gs.import_onnx(encoder)


    Not_30 = Relu_38 = Transpose_51 = Reshape_60 = Slice_74 = Slice_79 = Slice_84 = None
    for node in graph.nodes:
        if node.op == 'Not' and node.name == 'Not_30':
            Not_30 = node
        if node.op == 'Relu' and node.name == 'Relu_38':
            Relu_38 = node
        if node.op == 'Transpose' and node.name == 'Transpose_51':
            Transpose_51 = node
        if node.op == 'Reshape' and node.name == 'Reshape_60':
            Reshape_60 = node
        if node.op == 'Slice' and node.name == "Slice_74":
            Slice_74 = node
        if node.op == 'Slice' and node.name == "Slice_79":
            Slice_79 = node
        if node.op == 'Slice' and node.name == "Slice_84":
            Slice_84 = node

    Slice_79.inputs[2] = gs.Constant(name=Slice_79.inputs[2].name, values=np.array([-6], dtype=np.int64))
    Slice_79.inputs[4] = gs.Constant(name=Slice_79.inputs[4].name, values=np.array([4], dtype=np.int64))

    Cast0_input = Not_30.outputs[0]
    Cast0_output = gs.Variable(name="Cast0_output", dtype=np.dtype(np.int32), shape=None)

    Cast0 = gs.Node(name='Add_Cast0', op='Cast',
                    inputs=[Cast0_input],
                    outputs=[Cast0_output],
                    attrs=OrderedDict(to=6))
    graph.nodes.append(Cast0)

    Slice_79.inputs[0] = Cast0_output

    Cast1_input = Slice_79.outputs[0]
    Cast1_output = Slice_84.outputs[0]
    Slice_84.outputs.clear()
    Cast1 = gs.Node(name='Add_Cast1', op='Cast',
                    inputs=[Cast1_input],
                    outputs=[Cast1_output],
                    attrs=OrderedDict(to=9))
    graph.nodes.append(Cast1)

    table5000x256 = Slice_74.inputs[0].inputs[0].attrs['value'].values
    t4Tensor = Slice_74.inputs[2]
    j = 0
    for i in range(1, 24, 2):
        trashNode = Slice_74.o(i).o().o()
        factor256x256 = Slice_74.o(i).inputs[1].values
        newTable = table5000x256 @ factor256x256
        newTable = newTable.transpose().reshape(1, 4, 64, 5000)
        constantData = gs.Constant(f'Data-{j}', np.ascontiguousarray(newTable))
        sliceV = gs.Variable(f'sliceData-{j}', np.dtype(np.float32), [1, 4, 64, 't4'])
        zero = gs.Constant(name=f'Constant-0-{j}', values=np.array([0]))
        one = gs.Constant(name=f'Constant-1-{j}', values=np.array([1]))
        three = gs.Constant(name=f'Constant-3-{j}', values=np.array([3]))
        sliceN = gs.Node('Slice', f'SliceN-{j}',
                        inputs=[constantData, zero, t4Tensor, three, one],
                        outputs=[sliceV])
        j += 1
        graph.nodes.append(sliceN)
        Slice_74.o(i).o().o().o().inputs[1] = sliceV
        trashNode.outputs.clear()


    get_quant_nodes(graph)
    # Unsqueeze_29 = None
    # for node in graph.nodes:
    #     if node.op == 'Unsqueeze' and node.name == "Unsqueeze_29":
    #         Unsqueeze_29 = node
    #     if node.op == 'Not' and node.name == 'Not_30':
    #         Not_30 = node
    #     if node.op == 'Slice' and node.name == "Slice_79":
    #         Slice_79 = node
    #     if node.op == 'Slice' and node.name == "Slice_84":
    #         Slice_84 = node
    # start_node = Unsqueeze_29.outputs[0]
    # Unsqueeze_29_Cast_output = gs.Variable(name="Unsqueeze_29_Cast_output", dtype=None, shape=None)
    # attrs_dict = {}
    # attrs_dict['to'] = 6
    # newNode = gs.Node(name="Slice_79_Cast", op="Cast", inputs=[start_node],
    #                   outputs=[Unsqueeze_29_Cast_output], attrs=attrs_dict)
    # graph.nodes.append(newNode)  # 记得把新节点加入计算图中

    # Slice_79.inputs[0] = Unsqueeze_29_Cast_output
    # Slice_84_outputs = Not_30.outputs[0]
    # end_node = Slice_84.outputs[0]
    # Not_30.outputs[0] = end_node
    # Slice_84.outputs[0] = Slice_84_outputs
    # Not_30.inputs[0] = Slice_84.outputs[0]

    # Slice_84_Cast_output = gs.Variable(name="Slice_84_Cast_output", dtype=None, shape=None)
    # attrs_dict = {}
    # attrs_dict['to'] = 9
    # newNode = gs.Node(name="Slice_84_Cast", op="Cast", inputs=[Slice_84_outputs ],
    #                   outputs=[Slice_84_Cast_output], attrs=attrs_dict)
    # graph.nodes.append(newNode)  # 记得把新节点加入计算图中
    # Not_30.inputs[0] = Slice_84_Cast_output
    graph.cleanup().toposort()
    onnx.save(gs.export_onnx(graph), "encoder_new.onnx")
    pass
if __name__ == '__main__':
    wenet_encoder()
