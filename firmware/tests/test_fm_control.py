import math, unittest
def n_for(khz):
    if khz<76000 or khz>108000 or khz%100: raise ValueError
    return khz//100
def kidx(n,kv): return max(0,min(127,round(127*math.log((kv/n)/(10/1080))/math.log((20/760)/(10/1080)))))
def adf_n(n,acq): return (int(acq)<<21)|(n<<8)|1
class T(unittest.TestCase):
 def test_channels(self): self.assertEqual([n_for(x) for x in (76000,92000,108000)],[760,920,1080])
 def test_reject(self):
  for x in (75900,92050,108100): self.assertRaises(ValueError,n_for,x)
 def test_kidx(self): self.assertEqual((kidx(1080,10),kidx(920,15),kidx(760,20)),(0,69,127))
 def test_adf(self): self.assertEqual((adf_n(760,1),adf_n(920,1),adf_n(1080,1)),(0x22f801,0x239801,0x243801))
if __name__=='__main__': unittest.main(verbosity=2)
