"""Content-checked emitted instruction records; never executable cache data."""
import hashlib,json,sqlite3,zlib

class InstructionCache:
    def __init__(self,path,namespace):
        self.connection=sqlite3.connect(path)
        self.connection.execute('CREATE TABLE IF NOT EXISTS pages (key TEXT PRIMARY KEY, payload BLOB NOT NULL, checksum TEXT NOT NULL)')
        self.namespace=namespace;self.hits=0;self.misses=0
    def key(self,offsets):
        return hashlib.sha256((self.namespace+json.dumps(offsets,separators=(',',':'))).encode()).hexdigest()
    def get(self,offsets):
        key=self.key(offsets);row=self.connection.execute('SELECT payload,checksum FROM pages WHERE key=?',(key,)).fetchone()
        if row:
            try:
                payload=zlib.decompress(row[0])
                if hashlib.sha256(payload).hexdigest()!=row[1]:raise ValueError('checksum')
                records=json.loads(payload)
                if not isinstance(records,list) or len(records)!=len(offsets):raise ValueError('count')
                for offset,record in zip(offsets,records):
                    if not isinstance(record,dict) or record.get('offset')!=offset:raise ValueError('offset')
                    if not isinstance(record.get('instruction'),str) or not isinstance(record.get('indirect'),bool):raise ValueError('metadata')
                    if not isinstance(record.get('statements'),list) or not all(isinstance(s,str) for s in record['statements']):raise ValueError('statements')
                    if not isinstance(record.get('edges'),list) or not all(type(e) is int for e in record['edges']):raise ValueError('edges')
                self.hits+=1;return records
            except (ValueError,TypeError,zlib.error,UnicodeError):
                self.connection.execute('DELETE FROM pages WHERE key=?',(key,))
        self.misses+=1;return None
    def put(self,offsets,records):
        payload=json.dumps(records,separators=(',',':')).encode()
        self.connection.execute('INSERT OR REPLACE INTO pages VALUES (?,?,?)',(self.key(offsets),zlib.compress(payload,1),hashlib.sha256(payload).hexdigest()))
    def close(self):
        self.connection.commit();self.connection.close()
