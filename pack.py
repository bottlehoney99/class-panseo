import zipfile, unicodedata, sys, os
ver = sys.argv[1]
N = lambda x: unicodedata.normalize('NFC', x)
readme = open(N('사용법.txt'), encoding='utf-8').read()
os.makedirs('dist', exist_ok=True)
zname = N(f'dist/화면판서_v{ver}.zip')
with zipfile.ZipFile(zname, 'w', zipfile.ZIP_DEFLATED) as z:
    z.write('Panseo.exe', N('화면판서/Panseo.exe'))
    z.writestr(N('화면판서/사용법.txt'), ('﻿' + readme.replace('\n', '\r\n')).encode('utf-8'))
    for f in ['panseo.cpp', 'app.rc', 'app.manifest', 'app.ico', 'build.sh', 'pack.py']:
        z.write(f, N('화면판서/소스/' + f))
for i in zipfile.ZipFile(zname).infolist(): print(i.filename, bool(i.flag_bits & 0x800), i.file_size)
