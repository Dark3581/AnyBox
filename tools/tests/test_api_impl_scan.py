import unittest

import helpers

import api_impl_scan

LISTING = """
   180001000:\t48 8d 15 00 00 00 00 \tlea    rdx,[rip+0x0]        # 0x180009000
   180001007:\tff 15 00 00 00 00    \tcall   QWORD PTR [rip+0x0]        # 0x18000a000
   18000100d:\t48 89 05 00 00 00 00 \tmov    QWORD PTR [rip+0x0],rax        # 0x18000b000
   180001014:\tc3                   \tret
   180001015:\tcc                   \tint3
   180001020:\te9 00 00 00 00       \tjmp    0x180002000
   180001025:\tcc                   \tint3
   180001030:\te9 00 00 00 00       \tjmp    0x180003000
   180001035:\tcc                   \tint3
   180001040:\te9 00 00 00 00       \tjmp    0x180004000
   180001045:\tcc                   \tint3
   180002000:\t48 83 ec 28          \tsub    rsp,0x28
   180002004:\t48 8b 05 00 00 00 00 \tmov    rax,QWORD PTR [rip+0x0]        # 0x18000b000
   18000200b:\tff d0                \tcall   rax
   18000200d:\tc3                   \tret
   18000200e:\tcc                   \tint3
   180003000:\t48 8b c4             \tmov    rax,rsp
   180003003:\t4c 8d 40 18          \tlea    r8,[rax+0x18]
   180003007:\t48 8d 0d 00 00 00 00 \tlea    rcx,[rip+0x0]        # 0x180009100
   18000300e:\t48 8b d1             \tmov    rdx,rcx
   180003011:\te8 00 00 00 00       \tcall   0x180001020
   180003016:\t48 8b 4c 24 50       \tmov    rcx,QWORD PTR [rsp+0x50]
   18000301b:\t48 8b 01             \tmov    rax,QWORD PTR [rcx]
   18000301e:\t4c 8b cb             \tmov    r9,rbx
   180003021:\t44 8b c7             \tmov    r8d,edi
   180003024:\t48 8b d6             \tmov    rdx,rsi
   180003027:\t48 8b 40 50          \tmov    rax,QWORD PTR [rax+0x50]
   18000302b:\te8 00 00 00 00       \tcall   0x180005000
   180003030:\tc3                   \tret
   180003031:\tcc                   \tint3
   180004000:\t48 8b c4             \tmov    rax,rsp
   180004003:\t4c 8d 40 18          \tlea    r8,[rax+0x18]
   180004007:\t48 8d 15 00 00 00 00 \tlea    rdx,[rip+0x0]        # 0x180009120
   18000400e:\t48 8d 0d 00 00 00 00 \tlea    rcx,[rip+0x0]        # 0x180009110
   180004015:\te8 00 00 00 00       \tcall   0x180002000
   18000401a:\t48 8b 4c 24 50       \tmov    rcx,QWORD PTR [rsp+0x50]
   18000401f:\t48 8b 01             \tmov    rax,QWORD PTR [rcx]
   180004022:\t48 89 7c 24 20       \tmov    QWORD PTR [rsp+0x20],rdi
   180004027:\t48 8b d6             \tmov    rdx,rsi
   18000402a:\t48 8b 40 18          \tmov    rax,QWORD PTR [rax+0x18]
   18000402e:\te8 00 00 00 00       \tcall   0x180005000
   180004033:\tc3                   \tret
   180004034:\tcc                   \tint3
   180006000:\te8 00 00 00 00       \tcall   0x180001030
   180006005:\te8 00 00 00 00       \tcall   0x180001030
   18000600a:\te8 00 00 00 00       \tcall   0x180003000
   18000600f:\tc3                   \tret
"""

GUIDS = {0x180009100: "threading", 0x180009110: "provider", 0x180009120: "interface"}


class ApiImplScanTest(unittest.TestCase):
    def setUp(self):
        self.rows = api_impl_scan.parse_listing(LISTING)

    def test_wrapper_from_the_query_api_impl_lookup(self):
        self.assertEqual(api_impl_scan.find_wrapper(self.rows, 0x180009000), 0x180002000)
        self.assertIsNone(api_impl_scan.find_wrapper(self.rows, 0x180009999))

    def test_stubs_are_only_padded_jumps(self):
        self.assertEqual(api_impl_scan.stub_targets(self.rows), {0x180001020: 0x180002000, 0x180001030: 0x180003000, 0x180001040: 0x180004000})

    def test_thunks(self):
        thunks = api_impl_scan.scan(self.rows, 0x180002000, GUIDS.get)
        self.assertEqual(len(thunks), 2)
        same, pair = sorted(thunks, key=lambda t: t["thunk"])
        self.assertEqual((same["provider"], same["interface"], same["slot"], same["thunk"]), ("threading", "threading", 10, 0x180003000))
        self.assertEqual(same["callers"], 3, "two calls through the stub and one direct; the stub's own jump is not a caller")
        self.assertEqual(same["arguments"], ["r9", "r8d", "rdx"])
        self.assertEqual((pair["provider"], pair["interface"], pair["slot"], pair["callers"]), ("provider", "interface", 3, 0))
        self.assertEqual(pair["arguments"], ["qword@20", "rdx"])

    def test_summary(self):
        interfaces = api_impl_scan.summarize(api_impl_scan.scan(self.rows, 0x180002000, GUIDS.get))
        threading = next(i for i in interfaces if i["provider"] == "threading")
        self.assertEqual(threading["slots"], [10])
        self.assertEqual(threading["used"], {"10": {"callers": 3, "arguments": ["r9", "r8d", "rdx"]}})
        unused = next(i for i in interfaces if i["provider"] == "provider")
        self.assertEqual(unused["used"], {})


if __name__ == "__main__":
    unittest.main()
