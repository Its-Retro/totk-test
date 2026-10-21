"""Read the small external-edge registry from a large translation manifest."""
import json
import os


def read_external_edges(path):
    marker=b'"external_edges":'
    with path.open('rb') as stream:
        stream.seek(0,os.SEEK_END)
        stream.seek(max(0,stream.tell()-1048576))
        tail=stream.read()
    at=tail.rfind(marker)
    if at<0:
        # The full manifest remains the authority if its tail ever grows.
        return json.loads(path.read_text())['external_edges']
    edges,_=json.JSONDecoder().raw_decode(tail[at+len(marker):].decode('utf-8'))
    if not isinstance(edges,list) or not all(type(edge) is int for edge in edges):
        raise ValueError('Invalid external-edge provenance')
    return edges
