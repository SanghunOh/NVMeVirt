import re
import subprocess
import argparse
import struct

def parse_struct(struct_definition):
    # 필드 정의를 추출하기 위한 정규 표현식
    field_pattern = re.compile(r'__u(\d+)\s+(\w+);')
    fields = field_pattern.findall(struct_definition)
    
    return fields

def get_field_size(type_str):
    # __u32와 같은 타입의 크기를 계산
    type_size_map = {
        '8': 1,  # __u8
        '16': 2, # __u16
        '32': 4, # __u32
        '64': 8  # __u64
    }
    
    return type_size_map.get(type_str, 0)

def bytes_to_int(byte_list, endian='little'):
    # 바이트 문자열 생성
    byte_str = ''.join(byte_list)
    
    # 바이트 배열을 바이트 객체로 변환
    byte_obj = bytes.fromhex(byte_str)
    
    # 바이트 객체를 정수로 변환
    return int.from_bytes(byte_obj, endian)

def next_power_of_two(n):
    if (n & (n - 1)) == 0:
        return n
    
    if n == 0:
        return 1
    p = 1
    while p < n:
        p *= 2
    return p

parser = argparse.ArgumentParser(description='Parse a struct definition and print field sizes.')
parser.add_argument('struct_definition', metavar='S', type=str, 
                    help='Struct definition string (e.g., "__u32 freeblock_count; __u32 percentage_used; ...")')
args = parser.parse_args()
    
struct_definition = args.struct_definition

fields = parse_struct(struct_definition)

total_size = 0
for i in range(len(fields)):
    size = get_field_size(fields[i][0])
    total_size = total_size + size
    name = fields[i][1]
    fields[i] = (size, name)

log_data = subprocess.run(["sudo", "nvme", "get-log", "/dev/nvme1n1", "-i", "0xc0", "-l", "%d"  % (next_power_of_two(total_size)), "-n", "1"], stdout=subprocess.PIPE)
log_data = log_data.stdout.decode('utf-8')

# 정규 표현식을 사용하여 로그 데이터에서 바이트 값을 추출합니다.
lines = log_data.strip().split('\n')[2:]
filtered_log_data = '\n'.join(lines)

for i in range(len(lines)):
    lines[i] = lines[i][6:54]

lines = '\n'.join(lines)

bytes_list = lines.split()

# 4바이트 단위로 그룹화하여 배열에 저장
byte_groups = [bytes_list[i:i+4] for i in range(0, len(bytes_list), 4)]

# 각 그룹을 변수 배열로 저장
variables = [f'{" ".join(group)}' for group in byte_groups]


# 출력
for i in range(len(fields)):
    print("%-20s: %d" % (fields[i][1], bytes_to_int(variables[i])))