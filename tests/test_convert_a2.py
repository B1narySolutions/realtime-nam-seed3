import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('convert_a2', 'scripts/convert_a2.py')
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)


class ConvertA2Test(unittest.TestCase):
    def test_rejects_other_architectures_and_corrupt_data(self):
        model = json.loads(Path('models/local/fender-twin65-a2-lite.nam').read_text())
        cases = []
        for key, value in [('architecture', 'LSTM'), ('version', '0.6.0'),
                           ('sample_rate', 44100), ('weights', [0.0]),
                           ('weights', [float('nan')] * 1871)]:
            cases.append(dict(model, **{key: value}))
        for key, value in [('channels', 8), ('kernel_sizes', [6] * 23),
                           ('gating_mode', ['gated'] * 23),
                           ('activation', ['Tanh'] * 23)]:
            bad = copy.deepcopy(model)
            bad['config']['layers'][0][key] = value
            cases.append(bad)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad.nam'
            for bad in cases:
                path.write_text(json.dumps(bad))
                with self.assertRaises(ValueError):
                    converter.convert(path)

    def test_rejects_mismatched_head_scale(self):
        model = json.loads(Path('models/local/fender-twin65-a2-lite.nam').read_text())
        model['config']['head_scale'] *= 2
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad.nam'
            path.write_text(json.dumps(model))
            with self.assertRaises(ValueError):
                converter.convert(path)

    def test_extracts_lite_submodel(self):
        lite = json.loads(Path('models/local/fender-twin65-a2-lite.nam').read_text())
        bare = {key: value for key, value in lite.items() if key not in ('sample_rate', 'metadata')}
        other = dict(bare, config=dict(bare['config'], layers=[]))
        container = dict(architecture='SlimmableContainer', sample_rate=48000,
                         metadata=lite['metadata'],
                         config=dict(submodels=[dict(max_value=0.25, model=other),
                                                dict(max_value=0.5, model=bare)]))
        extracted = converter.extract(container, 'container')
        self.assertEqual(extracted['weights'], lite['weights'])
        self.assertEqual(extracted['sample_rate'], 48000)
        self.assertEqual(extracted['metadata'], lite['metadata'])

        bad_cases = [dict(container, architecture='WaveNet'),
                     dict(container, config=dict(submodels=[])),
                     dict(container, config=dict(submodels=[dict(max_value=0.5, model=bare)] * 2)),
                     dict(container, config=dict(submodels=[dict(max_value=0.5, model=other)]))]
        for bad in bad_cases:
            with self.assertRaises(ValueError):
                converter.extract(bad, 'container')

    def test_all_required_models(self):
        for amp in converter.load_manifest():
            packed, digest = converter.convert(Path('models/local') / amp['file'])
            self.assertEqual(len(packed), converter.WEIGHT_COUNT)
            self.assertEqual(len(digest), 64)


if __name__ == '__main__':
    unittest.main()
