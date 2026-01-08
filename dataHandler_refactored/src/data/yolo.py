import cv2
import numpy as np

def load_model(cfg):
    path = cfg["base_path"]
    name = cfg["model_name"]
    weight = "".join([path, name, '.weights'])
    cfg = "".join([path, name, '.cfg'])
    name = "".join([path, name, '.names'])

    net = cv2.dnn.readNet(weight, cfg)
    net.setPreferableBackend(cv2.dnn.DNN_BACKEND_CUDA)
    net.setPreferableTarget(cv2.dnn.DNN_TARGET_CUDA)
    classes = []
    with open(name, "r", encoding="UTF8") as f:
        classes = [line.strip() for line in f.readlines()]

    layers_names = net.getLayerNames()
    output_layers = [layers_names[i[0]-1]
                     for i in net.getUnconnectedOutLayers()]
    return net, classes, output_layers

def detect_objects(img, net, outputLayers, dim):
    blob = cv2.dnn.blobFromImage(
        img, scalefactor=0.00392, size=dim, mean=(0, 0, 0), swapRB=True, crop=False)

    net.setInput(blob)
    outputs = net.forward(outputLayers)
    return blob, outputs

def get_box_dimensions(outputs, dim):
    boxes = []
    confs = []
    class_ids = []
    height, width = dim

    for output in outputs:
        for detect in output:
            scores = detect[5:]
            class_id = np.argmax(scores)
            conf = scores[class_id]
            if conf > 0.15:
                center_x = int(detect[0] * width)
                center_y = int(detect[1] * height)
                w = int(detect[2] * width)
                h = int(detect[3] * height)
                x = int(center_x - w/2)
                y = int(center_y - h / 2)
                boxes.append([x, y, w, h])
                confs.append(float(conf))
                class_ids.append(class_id)
    return boxes, confs, class_ids