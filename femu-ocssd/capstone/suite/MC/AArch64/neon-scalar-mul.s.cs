# CS_ARCH_ARM64, 0, None
0x6a,0xb5,0x6c,0x5e = sqdmulh h10, h11, h12
0xb4,0xb6,0xa2,0x5e = sqdmulh s20, s21, s2
0x6a,0xb5,0x6c,0x7e = sqrdmulh h10, h11, h12
0xb4,0xb6,0xa2,0x7e = sqrdmulh s20, s21, s2
0xd4,0xde,0x2f,0x5e = fmulx s20, s22, s15
0x77,0xdd,0x61,0x5e = fmulx d23, d11, d1
0x71,0x93,0x6c,0x5e = sqdmlal s17, h27, h12
0x13,0x93,0xac,0x5e = sqdmlal d19, s24, s12
0x8e,0xb1,0x79,0x5e = sqdmlsl s14, h12, h25
0xec,0xb2,0xad,0x5e = sqdmlsl d12, s23, s13
0xcc,0xd2,0x6c,0x5e = sqdmull s12, h22, h12
0xcf,0xd2,0xac,0x5e = sqdmull d15, s22, s12
