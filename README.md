# 화면 판서

수업 시간에 PPT, 영상, 웹페이지 위에 바로 쓰고 지우는 Windows용 판서 도구입니다. 설치 없이 exe 하나로 실행됩니다.

## 내려받기

**[여기를 눌러 내려받기 (panseo.zip, v1.1)](https://github.com/bottlehoney99/class-panseo/raw/main/download/panseo.zip)**

받은 zip의 압축을 풀고 `Panseo.exe`를 실행하세요. 설치는 필요 없습니다.

서명이 없는 프로그램이라 처음 실행할 때 "Windows의 PC 보호" 창이 뜰 수 있습니다. **추가 정보** → **실행**을 누르면 됩니다.

## 기능

- 펜, 형광펜, 직선, 화살표, 사각형, 원, 글상자(한글 입력)
- 획 단위 지우개 (판서 중 오른쪽 버튼으로 끌어도 지워짐)
- 되돌리기/다시 실행, 전체 삭제
- 판서/마우스 전환: 판서를 꺼도 그림은 남고 아래 화면을 조작할 수 있음
- 도구 막대와 단축키 (`Ctrl+Shift+D` 전환, `Ctrl+Shift+X` 전체 삭제)

자세한 단축키는 [사용법.txt](사용법.txt)에 있습니다.

## 빌드

리눅스에서 MinGW-w64로 크로스 컴파일합니다.

```sh
sudo apt install mingw-w64
./build.sh            # Panseo.exe 생성
python3 pack.py 1.1   # dist/화면판서_v1.1.zip 생성 → download/panseo.zip으로 복사해 올리면 내려받기 링크가 갱신됨
```

Windows 10/11 64비트에서 동작합니다.
