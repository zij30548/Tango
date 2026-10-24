"""模型边界使用隔离 HTTPX 假上游，不读取密钥、不调用真实模型。"""
import asyncio
import json
import httpx
import pytest
from tango_backend.model_transport import createChatTransport
from tango_backend.errors import ApiError
from tango_backend.model import validateInput,createModel
from tango_backend.radio_model import validateRadioSelectInput,createRadioModel
from tango_backend.assistant_model import validateAssistantPlan
from tango_backend.task_model import rankingInput,createTaskModel
from tango_backend.compat import digest


def envelope(value,**extra):return {'choices':[{'finish_reason':'stop','message':{'content':json.dumps(value,ensure_ascii=False)}}],**extra}
def options(handler):return {'baseUrl':'http://127.0.0.1:9/v1','apiKey':'fixture-only-key','model':'qwen-fixture','client':httpx.AsyncClient(transport=httpx.MockTransport(handler))}

@pytest.mark.parametrize('url',['http://example.com/v1','https://a.test/v1/chat/completions','https://u:p@a.test/v1','https://a.test/v1?q=1','https://a.test/v1#bad','ftp://a.test/v1'])
def test_provider_configuration_rejects_unsafe_urls(url):
    with pytest.raises(ValueError):createChatTransport({'apiKey':'fixture-key','baseUrl':url})

async def test_transport_payload_and_redirect_not_followed():
    seen=[]
    def handle(request):
        seen.append(json.loads(request.content));return httpx.Response(302,headers={'location':'http://elsewhere.invalid'},json={})
    opts=options(handle);transport=createChatTransport(opts)
    with pytest.raises(ApiError) as error:await transport.complete({'prompt':'系统规则','input':{'instruction':'读书'}})
    assert error.value.code=='MODEL_UNAVAILABLE';assert len(seen)==1
    assert seen[0]['enable_thinking'] is False and seen[0]['messages'][1]['content']=='{"instruction":"读书"}'
    await opts['client'].aclose()

@pytest.mark.parametrize('status,content,headers,code',[(503,b'private error',{},'MODEL_UNAVAILABLE'),(200,b'not json',{},'INVALID_MODEL_RESPONSE'),(200,b'{}',{'content-length':'262145'},'MODEL_RESPONSE_TOO_LARGE'),(200,b' '*262145,{},'MODEL_RESPONSE_TOO_LARGE')])
async def test_transport_errors_are_bounded_and_redacted(status,content,headers,code):
    opts=options(lambda request:httpx.Response(status,content=content,headers=headers))
    with pytest.raises(ApiError) as error:await createChatTransport(opts).complete({'prompt':'规则','input':{}})
    assert error.value.code==code and 'private error' not in error.value.message
    await opts['client'].aclose()

async def test_transport_timeout_and_cancellation_stop_request():
    ended=asyncio.Event()
    async def handle(request):
        try:await asyncio.sleep(60)
        finally:ended.set()
    opts=options(handle);opts['timeoutMs']=10
    with pytest.raises(ApiError) as error:await createChatTransport(opts).complete({'prompt':'规则','input':{}})
    assert error.value.code=='MODEL_TIMEOUT' and ended.is_set()
    ended.clear();opts['timeoutMs']=1000;signal=asyncio.Event()
    task=asyncio.create_task(createChatTransport(opts).complete({'prompt':'规则','input':{}},{'signal':signal}))
    await asyncio.sleep(.01);signal.set()
    with pytest.raises(ApiError) as error:await task
    assert error.value.code=='REQUEST_CANCELLED' and ended.is_set()
    await opts['client'].aclose()

async def test_focus_strict_output_and_unsupported():
    current={'minutes':25,'scene':'work','source':'queue','excludeLive':False,'instrumentalOnly':False,'excludedArtists':[]}
    input=validateInput({'instruction':' 工作25分钟 ','currentRequest':current})
    opts=options(lambda request:httpx.Response(200,json=envelope({**current,'unsupported':[]})))
    assert await createModel(opts).parse(input)==current
    await opts['client'].aclose()
    opts=options(lambda request:httpx.Response(200,json=envelope({**current,'unsupported':['定时开始']})))
    with pytest.raises(ApiError) as error:await createModel(opts).parse(input)
    assert error.value.code=='UNSUPPORTED_REQUEST';await opts['client'].aclose()

async def test_radio_reasons_are_grounded_and_unknown_ids_rejected():
    base={'instruction':'阅读','history':[],'preference':'','candidates':[{'id':123,'title':'真实歌名','artist':'真实歌手','album':'真实专辑','sourceName':'阅读','sourceTags':['安静']}],'excludedIds':[]}
    input=validateRadioSelectInput(base)
    opts=options(lambda request:httpx.Response(200,json=envelope({'title':'阅读电台','note':'检测无人声','tracks':[{'id':123,'reason':'BPM为80'}]})))
    result=await createRadioModel(opts).select(input)
    assert 'BPM' not in result['tracks'][0]['reason'] and '真实歌名' in result['tracks'][0]['reason'];await opts['client'].aclose()
    opts=options(lambda request:httpx.Response(200,json=envelope({'title':'电台','note':'','tracks':[{'id':999,'reason':'造曲目'}]})))
    with pytest.raises(ApiError):await createRadioModel(opts).select(input)
    await opts['client'].aclose()

@pytest.mark.parametrize('instruction',['不要记住我喜欢爵士','他说“记住我喜欢爵士”','今晚喝酒','能不能记住我喜欢爵士？'])
def test_legacy_memory_permissions(instruction):
    with pytest.raises(ApiError):validateAssistantPlan({'kind':'remember','preference':'爵士','note':''},{'instruction':instruction})

def test_ranking_payload_hides_catalog_ids():
    input={'brief':{'title':'曲目987654','removeIds':[987654]},'candidates':[{'id':987654,'title':'曲目987654','artist':'歌手','durationMs':180000}],'lockedIds':[987654],'previousOrder':[987654]}
    projected=rankingInput(input)
    assert '987654' not in json.dumps(projected,ensure_ascii=False)
    assert projected['lockedIndices']==[0] and projected['candidates'][0]['index']==0

@pytest.mark.parametrize('value',[{'order':[1,0]},{'order':[0,0]},{'order':['0']},{'order':[999]},{'order':[]},{'ids':[123]},None])
async def test_ranking_mapping_or_explicit_degradation(value):
    opts=options(lambda request:httpx.Response(200,json=envelope(value)))
    result=await createTaskModel(opts).compose({'brief':{},'candidates':[{'id':123},{'id':456}]})
    expected=[456,123] if value=={'order':[1,0]} else [123,456]
    assert result['value']['ids']==expected
    assert result['value']['ranking']['status']==('model' if expected==[456,123] else 'degraded')
    await opts['client'].aclose()

def test_cross_language_digest_known_vectors():
    assert digest({'b':[True,'粤语😀'],'a':1})==digest({'a':1.0,'b':[True,'粤语😀']})

@pytest.mark.parametrize('raw',[b'{"x":NaN}',b'{"x":Infinity}',b'{"x":-Infinity}'])
async def test_transport_rejects_non_json_numbers(raw):
    opts=options(lambda request:httpx.Response(200,content=raw))
    with pytest.raises(ApiError) as error:await createChatTransport(opts).complete({'prompt':'规则','input':{}})
    assert error.value.code=='INVALID_MODEL_RESPONSE';await opts['client'].aclose()

@pytest.mark.parametrize('field,value',[('tool_calls',[]),('function_call',{}),('refusal',{})])
def test_task_envelope_rejects_javascript_truthy_empty_containers(field,value):
    from tango_backend.task_model import jsonEnvelope
    e=envelope({'order':[0]});e['choices'][0]['message'][field]=value
    with pytest.raises(ApiError) as error:jsonEnvelope(e)
    assert error.value.code=='INVALID_TASK_MODEL' and error.value.taskDiagnostic['reasonCounts']=={'invalidEnvelope':1}

@pytest.mark.parametrize('raw',['{"order":[NaN]}','{"order":[Infinity]}'])
def test_task_content_rejects_non_json_constants(raw):
    from tango_backend.task_model import jsonEnvelope
    e=envelope({});e['choices'][0]['message']['content']=raw
    with pytest.raises(ApiError):jsonEnvelope(e)

@pytest.mark.parametrize('identifier,title,expected',[(987654.0,'曲目987654','曲目候选[0]'),(987654,'曲目987654١','曲目候选[0]١')])
def test_ranking_projection_uses_js_number_and_ascii_digit_semantics(identifier,title,expected):
    projected=rankingInput({'brief':{},'candidates':[{'id':identifier,'title':title}]})
    assert projected['candidates'][0]['title']==expected

def test_digest_matches_node_canonical_json_for_unicode_numeric_and_fractional_data():
    # 期望值由旧 Node 源码独立生成并冻结；删除旧后台后也能验证跨语言摘要契约。
    from pathlib import Path
    fixture=json.loads((Path(__file__).parent/'fixtures/digest_js_v1.json').read_text())
    assert [digest(case['input']) for case in fixture['cases']]==[case['digest'] for case in fixture['cases']]
