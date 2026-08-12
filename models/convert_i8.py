import sys
import os
from rknn.api import RKNN

def parse_arg():
    if len(sys.argv) < 4:
        print("Usage: python3 {} onnx_model_path platform calibration_dataset.txt [output_rknn_path]".format(sys.argv[0]))
        print("       platform choose from: rk3588, rk3562, rk3566, rk3568, rk3576")
        print("       calibration_dataset.txt: one image path per line (100-500 images from YOUR scene)")
        print("Example: python3 {} best_split.onnx rk3588 calib.txt best_int8.rknn".format(sys.argv[0]))
        exit(1)

    model_path = sys.argv[1]
    platform = sys.argv[2]
    dataset_path = sys.argv[3]

    if len(sys.argv) > 4:
        output_path = sys.argv[4]
    else:
        output_path = 'model_i8.rknn'

    return model_path, platform, dataset_path, output_path

if __name__ == '__main__':
    model_path, platform, dataset_path, output_path = parse_arg()

    if not os.path.exists(dataset_path):
        print("ERROR: Calibration dataset not found: {}".format(dataset_path))
        exit(1)
    if not os.path.exists(model_path):
        print("ERROR: ONNX model not found: {}".format(model_path))
        exit(1)

    # Count images
    with open(dataset_path) as f:
        lines = [l.strip() for l in f if l.strip() and not l.startswith('#')]
    print("Calibration images: {}".format(len(lines)))

    rknn = RKNN(verbose=True)

    print('--> Config model')
    rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]],
                target_platform=platform)
    print('done')

    print('--> Loading model')
    ret = rknn.load_onnx(model=model_path)
    if ret != 0:
        print('Load model failed!')
        exit(ret)
    print('done')

    print('--> Building model (INT8 quantization)')
    ret = rknn.build(do_quantization=True, dataset=dataset_path)
    if ret != 0:
        print('Build model failed!')
        exit(ret)
    print('done')

    print('--> Export rknn model')
    ret = rknn.export_rknn(output_path)
    if ret != 0:
        print('Export rknn model failed!')
        exit(ret)
    print('done')

    # Print output info
    print('Success: {}'.format(output_path))
    rknn.release()
