#! /usr/bin/env python3 -W ignore::DeprecationWarning
from os import times
import flatbuffers
import zstd
import sys
import json
import struct
import datetime

PREAMBLE_LENGTH = 8
PRETTY_PRINT_JSON=False


def fmt_payload(payload):
    payload = bytearray(payload).decode('utf-8')
    if PRETTY_PRINT_JSON == False:
        return payload
    return json.dumps(json.loads(payload), indent = 1)

def get_json(payload):
    payload = bytearray(payload).decode('utf-8')
    return json.loads(payload)

def parse_preamble(buf):
    reserved = buf[0]
    version = buf[1]
    msg_type = int.from_bytes(buf[2:4], "big")
    msg_size = int.from_bytes(buf[4:8], "big")
    return { 'reserved': reserved, 'version': version, 'msg_type': msg_type, 'msg_size': msg_size}


def enum_to_str(type, value):
    for k in [f for f in dir(type) if not f.startswith('__')]:
        if getattr(type, k) == value:
            return k
    return f'<unk_{value}>'

def payload_name(payload):
    return enum_to_str(PayloadType, payload)

def learning_mode_name(learning_mode):
    return enum_to_str(LearningModeType, learning_mode)

def event_encoding_name(batch_type):
    return enum_to_str(EventEncoding, batch_type)

def timestamp_to_datetime(timestamp):
    if timestamp == None:
        return None
    return datetime.datetime(timestamp.Year(), timestamp.Month(), timestamp.Day(), timestamp.Hour(), timestamp.Minute(), timestamp.Second(), timestamp.Subsecond())

# Similar hack to the C# one due to limited binding codegen
def getString(table):
    off = table.Pos
    length = flatbuffers.encode.Get(flatbuffers.number_types.UOffsetTFlags.packer_type, table.Bytes, off)
    start = off + flatbuffers.number_types.UOffsetTFlags.bytewidth
    return bytes(table.Bytes[start:start+length])

def cast(table, tmp_type):
    tmp = tmp_type()
    tmp.Init(table.Bytes, table.Pos)
    return tmp


def parse_cb(payload, verbose):
    evt = CbEvent.GetRootAsCbEvent(payload, 0)
    if not verbose:
        print(f'\tcb: actions:{evt.ActionIdsLength()} model:{evt.ModelId()} lm:{learning_mode_name(evt.LearningMode())} deferred:{evt.DeferredAction()}')
    else:
        print(f'\tcb: actions:{evt.ActionIdsAsNumpy()} probs: {evt.ProbabilitiesAsNumpy()} model:{evt.ModelId()} lm:{learning_mode_name(evt.LearningMode())} deferred:{evt.DeferredAction()}')
        print(f'\t\tcontext: {fmt_payload(evt.ContextAsNumpy())}')   

def fill_cb(payload, message):
    evt = CbEvent.GetRootAsCbEvent(payload, 0)
    message['actions'] = evt.ActionIdsAsNumpy()
    message['probs'] = evt.ProbabilitiesAsNumpy()
    message['model'] = evt.ModelId()
    message['learning_mode'] = learning_mode_name(evt.LearningMode())
    message['deferred'] = evt.DeferredAction()       

def parse_outcome(payload):
    evt = OutcomeEvent.GetRootAsOutcomeEvent(payload, 0)

    value = evt.Value()
    if evt.ValueType() == OutcomeValue.literal:
        value = getString(value)
    elif evt.ValueType() == OutcomeValue.numeric:
        value = cast(value, NumericOutcome).Value()

    index = evt.Index()
    if evt.IndexType() == OutcomeValue.literal:
        index = getString(index)
    elif evt.IndexType() == OutcomeValue.numeric:
        index = cast(index, NumericIndex).Index()

    print(f'\toutcome: value:{value} index:{index} action-taken:{evt.ActionTaken()}')

def fill_outcome(payload, message):
    evt = OutcomeEvent.GetRootAsOutcomeEvent(payload, 0)

    value = evt.Value()
    if evt.ValueType() == OutcomeValue.literal:
        value = getString(value)
    elif evt.ValueType() == OutcomeValue.numeric:
        value = cast(value, NumericOutcome).Value()

    index = evt.Index()
    if evt.IndexType() == OutcomeValue.literal:
        index = getString(index)
    elif evt.IndexType() == OutcomeValue.numeric:
        index = cast(index, NumericIndex).Index()

    message['reward'] = value
    message['index'] = index
    message['action_taken'] = evt.ActionTaken()

def parse_multislot(payload):
    evt = MultiSlotEvent.GetRootAsMultiSlotEvent(payload, 0)

    print(f'\tmulti-slot slots:{evt.SlotsLength()} model:{evt.ModelId()} deferred:{evt.DeferredAction()} has-baseline:{not evt.BaselineActionsIsNone()}')
    print(f'\t\tcontext: {fmt_payload(evt.ContextAsNumpy())}')
    if not evt.BaselineActionsIsNone():
        print(f'\t\tbaselines: {" ".join([str(b) for b in evt.BaselineActionsAsNumpy()])}')

def parse_multistep(payload):
    evt = MultiStepEvent.GetRootAsMultiStepEvent(payload, 0)

    print(f'\tmultistep: index: {evt.EventId()}\t actions:{evt.ActionIdsLength()} model:{evt.ModelId()}')
    print(f'\t\tcontext: {fmt_payload(evt.ContextAsNumpy())}')


def fill_multistep(payload, message):
    evt = MultiStepEvent.GetRootAsMultiStepEvent(payload, 0)
    c = get_json(evt.ContextAsNumpy())
    message['index'] = evt.EventId()
    message['actions'] = evt.ActionIdsAsNumpy()
    message['probs'] = evt.ProbabilitiesAsNumpy()
    message['model'] = evt.ModelId()

def parse_continuous_action(payload):
    evt = CaEvent.GetRootAsCaEvent(payload, 0)

    print(f'\tcontinuous-action: action:{evt.Action()} pdf-value:{evt.PdfValue()} deferred:{evt.DeferredAction()}')
    print(f'\t\tcontext: {fmt_payload(evt.ContextAsNumpy())}')

def parse_dedup_info(payload):
    evt = DedupInfo.GetRootAsDedupInfo(payload, 0)
    print(f'\tdedup-info ids:{evt.IdsLength()} values:{evt.ValuesLength()}')
    for i in range(0, evt.ValuesLength()):
        print(f'\t\t[{evt.Ids(i)}]: "{evt.Values(i).decode("utf-8")}"')

def dump_event(event_payload, idx, timestamp=None, verbose=False):
    evt = Event.GetRootAsEvent(event_payload, 0)
    m = evt.Meta()

    print(f'\t[{idx}] id:{m.Id().decode("utf-8")} type:{payload_name(m.PayloadType())} payload-size:{evt.PayloadLength()} encoding:{event_encoding_name(m.Encoding())} ts:{timestamp_to_datetime(timestamp)}')

    payload = evt.PayloadAsNumpy()
    if m.Encoding() == EventEncoding.Zstd:
        payload = zstd.decompress(evt.PayloadAsNumpy())

    if m.PayloadType() == PayloadType.CB:
        parse_cb(payload, verbose)
    elif m.PayloadType() == PayloadType.CCB or m.PayloadType() == PayloadType.Slates:
        parse_multislot(payload)
    elif m.PayloadType() == PayloadType.Outcome:
        parse_outcome(payload)
    elif m.PayloadType() == PayloadType.CA:
        parse_continuous_action(payload)
    elif m.PayloadType() == PayloadType.DedupInfo:
        parse_dedup_info(payload)
    elif m.PayloadType() == PayloadType.MultiStep:
        parse_multistep(payload)
    else:
        print('unknown payload type')

def dump_event_csv(event_payload, idx, timestamp=None, verbose=False):
    evt = Event.GetRootAsEvent(event_payload, 0)
    m = evt.Meta()
    message = {
        'id': m.Id().decode("utf-8"),
        'payload-size': evt.PayloadLength(),
        'encoding': event_encoding_name(m.Encoding()),
        't': timestamp_to_datetime(timestamp)
        }

    payload = evt.PayloadAsNumpy()
    if m.Encoding() == EventEncoding.Zstd:
        payload = zstd.decompress(evt.PayloadAsNumpy())

    if m.PayloadType() == PayloadType.CB:
        fill_cb(payload, message)
    elif m.PayloadType() == PayloadType.CCB or m.PayloadType() == PayloadType.Slates:
        ...
    elif m.PayloadType() == PayloadType.Outcome:
        fill_outcome(payload, message)
    elif m.PayloadType() == PayloadType.CA:
        ...
    elif m.PayloadType() == PayloadType.DedupInfo:
        ...
    elif m.PayloadType() == PayloadType.MultiStep:
        fill_multistep(payload, message)
    else:
        ...
    return {
        'type': payload_name(m.PayloadType()),
        'message': message
    }


def dump_event_batch(buf):
    batch = EventBatch.GetRootAsEventBatch(buf, 0)
    meta = batch.Metadata()
    enc = meta.ContentEncoding().decode('utf-8')
    print(f'event-batch evt-count:{batch.EventsLength()} enc:{enc}')
    is_dedup = b'DEDUP' == meta.ContentEncoding()
    for i in range(0, batch.EventsLength()):
        dump_event(batch.Events(i).PayloadAsNumpy(), i)
    print("----\n")


def dump_preamble_file(file_name, buf):
    while len(buf) > 8:
        preamble = parse_preamble(buf)
        print(f'parsing preamble file {file_name}\n\tpreamble:{preamble}')
        dump_event_batch(buf[PREAMBLE_LENGTH : PREAMBLE_LENGTH + preamble["msg_size"]])
        buf = buf[PREAMBLE_LENGTH + preamble["msg_size"]:]


MSG_TYPE_FILEMAGIC = 0x42465756
MSG_TYPE_HEADER = 0x55555555
MSG_TYPE_CHECKPOINT = 0x11111111
MSG_TYPE_REGULAR = 0xFFFFFFFF
MSG_TYPE_EOF = 0xAAAAAAAA

class JoinedLogStreamReader:
    def __init__(self, buf):
        self.buf = buf
        self.offset = 0

    def read(self, size):
        if size == 0:
            return bytearray([])
        data = self.buf[self.offset : self.offset + size]
        self.offset += size
        return data

    def read_message(self):
        if len(self.buf) <= self.offset:
            return None
        kind = struct.unpack('I', self.read(4))[0]
        length = struct.unpack('I', self.read(4))[0]
        # FILEMAGIC has inline payload, special case it here
        if kind == MSG_TYPE_FILEMAGIC:
            return (kind, length)

        payload = self.read(length)
        #discard padding
        self.read(length % 8)
        return (kind, payload)

    def messages(self):
        while True:
            msg = self.read_message()
            if msg == None or msg[0] == MSG_TYPE_EOF:
                break
            if msg[0] == MSG_TYPE_REGULAR:
                yield (msg[0], JoinedPayload.GetRootAsJoinedPayload(msg[1], 0))
            elif msg[0] == MSG_TYPE_CHECKPOINT:
                yield (msg[0], CheckpointInfo.GetRootAsCheckpointInfo(msg[1], 0))
            elif msg[0] == MSG_TYPE_HEADER:
                yield (msg[0], FileHeader.GetRootAsFileHeader(msg[1], 0))
            else:
                yield (msg[0], msg[1])

def dump_joined_log_file(file_name, buf, verbose):
    print(f'Parsing binary-log file:{file_name}')

    reader = JoinedLogStreamReader(buf)
    for msg in reader.messages():
        if msg[0] == MSG_TYPE_REGULAR:
            msg = msg[1]
            print(f'joined-batch events: {msg.EventsLength()}')
            for i in range(msg.EventsLength()):
                joined_event = msg.Events(i)
                dump_event(joined_event.EventAsNumpy(), i, joined_event.Timestamp(), verbose)
        elif msg[0] == MSG_TYPE_CHECKPOINT:
            checkpoint_info = msg[1]
            print('Parsing checkpoint info:')
            print(f'\treward function type is: {checkpoint_info.RewardFunctionType()}')
            print(f'\tdefault reward is: {checkpoint_info.DefaultReward()}')
            print(f'\tlearning mode config is: {checkpoint_info.LearningModeConfig()}')
            print(f'\tproblem type config is: {checkpoint_info.ProblemTypeConfig()}')            
        elif msg[0] == MSG_TYPE_HEADER:
            print(f'Parsing File Header:')
            header = msg[1]
            for i in range(header.PropertiesLength()):
                p = header.Properties(i)
                key = p.Key().decode('utf-8')
                value = p.Value().decode('utf-8')
                print('\t{key} :: {value}')
        elif msg[0] == MSG_TYPE_FILEMAGIC:
            print(f' File Version: {msg[1]}')
        else:
            print(f'unknown message type: {msg[0]}')

def get_records(file_name):
    buf = bytearray(open(file_name, 'rb').read())

    reader = JoinedLogStreamReader(buf)
    for msg in reader.messages():
        if msg[0] == MSG_TYPE_REGULAR:
            msg = msg[1]
            for i in range(msg.EventsLength()):
                joined_event = msg.Events(i)
                yield dump_event_csv(joined_event.EventAsNumpy(), i, joined_event.Timestamp())
        elif msg[0] == MSG_TYPE_CHECKPOINT:
            checkpoint_info = msg[1]
            yield  {'type': 'checkpoint', 
                'message': {
                    'reward_function': checkpoint_info.RewardFunctionType(),
                    'default_reward': checkpoint_info.DefaultReward(),
                    'learning_mode': checkpoint_info.LearningModeConfig(),
                    'problem_type': checkpoint_info.ProblemTypeConfig()}}       
        elif msg[0] == MSG_TYPE_HEADER:
            header = msg[1]
            yield {'type': 'header',
                'message': {header.Properties(i).Key().decode('utf-8') : header.Properties(i).Value().decode('utf-8') for i in range(header.PropertiesLength())}}
        elif msg[0] == MSG_TYPE_FILEMAGIC:
            yield {'type': 'magic', 'message': {'version': msg[1]}}
        else:
            yield {'type': 'unknown', 'message': None}

def is_binary_log_msg(buf):
    msg_id = struct.unpack('I', buf)[0]
    return msg_id == MSG_TYPE_FILEMAGIC or msg_id == MSG_TYPE_HEADER or msg_id == MSG_TYPE_CHECKPOINT or msg_id == MSG_TYPE_REGULAR or msg_id == MSG_TYPE_EOF

def dump_file(f, verbose=False):
    buf = bytearray(open(f, 'rb').read())

    if is_binary_log_msg(buf[0:4]):
        dump_joined_log_file(f, buf, verbose)
    else:
        dump_preamble_file(f, buf)


# Generate FB serializers if they are not available
try:
    import reinforcement_learning.messages.flatbuff.v2.EventBatch
except Exception as e:
    import pathlib
    import subprocess

    script_dir = pathlib.Path(__file__).parent.absolute()
    input_dir = pathlib.Path(script_dir).parents[1].joinpath('rlclientlib', 'schema', 'v2')

    input_files = " ".join([str(x) for x in input_dir.glob('*.fbs')])
    subprocess.run(f'flatc --python {input_files}', cwd=script_dir, shell=True, check=True)


# must be done after the above that generates the classes we're importing
from reinforcement_learning.messages.flatbuff.v2.EventBatch import EventBatch
from reinforcement_learning.messages.flatbuff.v2.Event import Event
from reinforcement_learning.messages.flatbuff.v2.EventEncoding import EventEncoding
from reinforcement_learning.messages.flatbuff.v2.LearningModeType import LearningModeType
from reinforcement_learning.messages.flatbuff.v2.PayloadType import PayloadType
from reinforcement_learning.messages.flatbuff.v2.OutcomeValue import OutcomeValue
from reinforcement_learning.messages.flatbuff.v2.NumericOutcome import NumericOutcome
from reinforcement_learning.messages.flatbuff.v2.NumericIndex import NumericIndex

from reinforcement_learning.messages.flatbuff.v2.CbEvent import CbEvent
from reinforcement_learning.messages.flatbuff.v2.OutcomeEvent import OutcomeEvent
from reinforcement_learning.messages.flatbuff.v2.MultiSlotEvent import MultiSlotEvent
from reinforcement_learning.messages.flatbuff.v2.CaEvent import CaEvent
from reinforcement_learning.messages.flatbuff.v2.DedupInfo import DedupInfo
from reinforcement_learning.messages.flatbuff.v2.MultiStepEvent import MultiStepEvent

from reinforcement_learning.messages.flatbuff.v2.FileHeader import *
from reinforcement_learning.messages.flatbuff.v2.JoinedEvent import *
from reinforcement_learning.messages.flatbuff.v2.JoinedPayload import *
from reinforcement_learning.messages.flatbuff.v2.CheckpointInfo import *
from reinforcement_learning.messages.flatbuff.v2.ProblemType import *


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('files', metavar='N', type=str, nargs='+', help='files to parse')
    parser.add_argument('--verbose', dest='verbose', action='store_true', help='verbose output')

    args = parser.parse_args()
    for input_file in args.files:
        dump_file(input_file, args.verbose)

if __name__ == "__main__":
    main()
