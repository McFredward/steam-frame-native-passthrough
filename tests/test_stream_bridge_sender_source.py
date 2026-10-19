#!/usr/bin/env python3
"""Optional private-source pin check. No Valve body is copied into this project."""
import argparse,hashlib,json,struct
from pathlib import Path
PIN="1e846be041039798580a91452371f6288c614fb4c152126b137e2c693fb77eef"
SPANS=[{'rva': '0x9db060', 'bytes': 7, 'sha256': 'f6829f2c8a8cc9a79656bb99ee79f1f3ff38542731d97039ab6a222479758db1', 'meaning': 'public GetProcAddress import tail'}, {'rva': '0xa1e660', 'bytes': 10, 'sha256': 'c3ba26ed2f399e2c39d9721a4e53e4c2df59c0d1b64c14c161496a60bd71fc96', 'meaning': 'table version11.1'}, {'rva': '0xa1e67c', 'bytes': 23, 'sha256': 'dd5b9c485f6a24162ed85bd38404988f121589d681feb787879f09f44fdc8047', 'meaning': 'caller-owned table copy2552'}, {'rva': '0xa1e698', 'bytes': 5, 'sha256': '29d9566456c32445b2fdc0cca97ca369f8d37f281e0034f22406cb6906c9331f', 'meaning': 'public factory call'}, {'rva': '0xa1df2d', 'bytes': 7, 'sha256': '39b33d3525013a16088a54403e2a5cdf3ca44bac772f3576206f294be971bb77', 'meaning': 'lock structure version11.1'}, {'rva': '0xa1df62', 'bytes': 6, 'sha256': '7f0a73121408804607c3bb587caca1615729819744c84359a4d5ed41b6b38eee', 'meaning': 'public Lock table0x90'}, {'rva': '0xa1e112', 'bytes': 19, 'sha256': '7ef9fabc44e856088788ce12514b6e9927d57f733ccb0543cff1f7509fde09cb', 'meaning': 'size and destination allocation'}, {'rva': '0xa1e137', 'bytes': 16, 'sha256': '655a4ed1b9f2df1343d7c4dd21454bd6fe97ac37a2716dd193bfa44489644682', 'meaning': 'complete returned byte copy'}, {'rva': '0xa1e147', 'bytes': 21, 'sha256': 'a792f4a4ecd5111ee2af5832b9070a7d96978508ed7b0ce718e9e1986e1ced53', 'meaning': 'original handle and public Unlock0x98'}, {'rva': '0xa1dd4a', 'bytes': 28, 'sha256': '3e45a98a012d5323785ce2cc8c3c7b718d0e4bb618719d577149d1fee4e766e1', 'meaning': 'copied AU into transmitter'}, {'rva': '0xa1e6f1', 'bytes': 6, 'sha256': 'f23d0a9ea98a43f7153b8b0872d22a1fef52acc03bf8b3249fe03acc9b8ae3dd', 'meaning': 'public OpenSessionEx'}]
def verify(path):
    data=Path(path).read_bytes()
    assert len(data)==13908632 and hashlib.sha256(data).hexdigest()==PIN,"wrong private driver source"
    pe=struct.unpack_from("<I",data,60)[0];opt=pe+24
    assert data[pe:pe+4]==b"PE\0\0" and struct.unpack_from("<H",data,pe+4)[0]==0x8664
    sections=[];sh=opt+struct.unpack_from("<H",data,pe+20)[0]
    for i in range(struct.unpack_from("<H",data,pe+6)[0]):
        vs,rva,size,raw=struct.unpack_from("<IIII",data,sh+i*40+8);sections.append((rva,size,raw))
    for span in SPANS:
        r=int(span["rva"],16);n=span["bytes"]
        matched=[data[raw+r-start:raw+r-start+n] for start,size,raw in sections if start<=r and r+n<=start+size]
        assert len(matched)==1 and hashlib.sha256(matched[0]).hexdigest()==span["sha256"],span["meaning"]
    return {"scope":"static source only; no hook execution", "driver_sha256":PIN,"checks":len(SPANS)+2,"spans":SPANS}
if __name__=="__main__":
    p=argparse.ArgumentParser();p.add_argument("private_driver");p.add_argument("--output",type=Path);a=p.parse_args()
    result=verify(a.private_driver)
    if a.output:a.output.write_text(json.dumps(result,indent=2)+"\n")
    print("sender private-source pins:",result["checks"],"PASS")
