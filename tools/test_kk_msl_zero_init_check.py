import subprocess, sys, tempfile, unittest, os
sys.path.insert(0, os.path.dirname(__file__))
import kk_msl_zero_init_check as c

OLD = """kernel void main_entrypoint()
{
    uint t1 = uint(0);
    bool t2 = bool(0);
    uint3 t3 = uint3(0);
    ulong t4 = ulong(0);
    uint r5 = uint(0);
    t1 = uint(7u);
    t2 = t1 != uint(0u);
}
"""
NEW = OLD.replace('t1 = uint(0);', 't1;').replace('t2 = bool(0);', 't2;').replace('t3 = uint3(0);', 't3;').replace('t4 = ulong(0);', 't4;')

class T(unittest.TestCase):
    def test_equal_modulo_init(self):
        self.assertIsNone(c.check(OLD, NEW))
    def test_register_init_must_stay(self):
        self.assertIsNotNone(c.check(OLD, NEW.replace('r5 = uint(0);', 'r5;')))
    def test_body_change_detected(self):
        self.assertIsNotNone(c.check(OLD, NEW.replace('uint(7u)', 'uint(8u)')))
    def test_new_with_init_detected(self):
        self.assertIsNotNone(c.check(OLD, OLD))
    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, 'a.msl'), os.path.join(d, 'b.msl')
            open(a, 'w').write(OLD); open(b, 'w').write(NEW)
            self.assertEqual(c.main([a, b]), 0)

if __name__ == '__main__':
    unittest.main()
