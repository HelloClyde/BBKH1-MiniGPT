"""Reproduce both model files without rewriting checked-in generated UI/tokenizer assets."""
import hashlib,json
import export_minigpt as q8
import export_minigpt_int4 as q4
EXPECTED={8:'825a306451e14cb2b483bebfd62d4947884f6ac84e4d407f900ed3de9b16052b',4:'b69ee5c66ee2b526e20790a987adcf18357e5c05054403ddfc6aa9a13dedb584'}
def main():
    q8.UPSTREAM.mkdir(parents=True,exist_ok=True);q8.OUT.mkdir(parents=True,exist_ok=True)
    meta=q8.weights();q8.download('tokenizer.json')
    (q8.OUT/'model.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (q8.OUT/'prompt.txt').write_text('你好，请简单介绍一下自己。',encoding='utf-8')
    q4.main()
    for bits,out,name in [(8,q8.OUT,'model.mg8'),(4,q4.OUT,'model.mg4')]:
        digest=hashlib.sha256((out/name).read_bytes()).hexdigest()
        assert digest==EXPECTED[bits],(name,digest)
        print('PASS pinned model',name,digest)
if __name__=='__main__':main()
