# mining_utils.py
import subprocess, json, ctypes, struct, time, binascii, hashlib, multiprocessing, argparse, sys, os, socket, threading, requests, traceback, secrets, psutil, inspect, re, portalocker
from multiprocessing import Lock, Process, Value

# Global cache for RPC settings
RPC_CONFIG = {'user': '', 'pass': '', 'port': '8332', 'initialized': False}

def update_stats_simple(stats_dict, height):

    if not stats_dict:
        print("[00:00:00] T:0.0MH/s [0:0.0] H:0")
        return

    current_total = 0.0
    compact_display = []
    detailed_lines = []
    
    # GPU
    for key in sorted(stats_dict.keys()):
        if key.startswith("GPU_"):
            gpu_id = key.replace("GPU_", "")
            data = stats_dict[key]
            
            if isinstance(data, dict):
                speed = data.get("speed", 0.0)
                temp = data.get("temperature")
                fan = data.get("fan_speed")
                pwr = data.get("power_usage")
                util = data.get("utilization")
                mem = data.get("memory_used")
                clock_core = data.get("clock_core")
                clock_memory = data.get("clock_memory")
            else:
                speed = float(data)
                temp = fan = pwr = util = mem = clock_core = clock_memory = None
            
            current_total += speed
            compact_display.append(f"G{gpu_id}:{speed:.1f}")
            
            details = []
            if temp is not None and temp > 0:
                details.append(f"{temp}°C")
            if fan is not None and fan > 0:
                try:
                    fan_val = int(float(fan))
                    details.append(f"Fan:{fan_val}%")
                except:
                    pass
            if pwr is not None and pwr > 0:
                details.append(f"{pwr:.0f}W")
            if util is not None and util > 0:
                details.append(f"Load:{util}%")
            if mem is not None and mem > 0:
                details.append(f"Mem:{mem}MB")
            if clock_core is not None and clock_memory is not None and clock_core > 0:
                details.append(f"Clk:{clock_core}/{clock_memory}MHz")
            
            detail_line = f"GPU{gpu_id}:{speed:.1f}MH/s"
            if details:
                detail_line += f" [{', '.join(details)}]"
            
            detailed_lines.append(detail_line)
    
    # CPU
    for key in sorted(stats_dict.keys()):
        if key.startswith("CPU_"):
            cpu_id = key.replace("CPU_", "")
            speed = float(stats_dict[key])
            current_total += speed
            compact_display.append(f"C{cpu_id}:{speed:.1f}")
    
    timestamp = time.strftime("%H:%M:%S")
    compact_str = " ".join(compact_display) if compact_display else "0:0.0"

    status_msg = f"[{timestamp}] T:{current_total:.1f}MH/s [{compact_str}] H:{height}"
    print(status_msg)
    for line in detailed_lines:
        print(f"   {line}")


def show_block_found_simple(header_data, status_text):
    found_nonce = header_data['found_nonce']
    stats_key = header_data['stats_key']
    
    if stats_key.startswith("GPU_"):
        device_type = "GPU"
        device_id = stats_key.replace("GPU_", "")
    else:
        device_type = "CPU" 
        device_id = stats_key.replace("CPU_", "")
    
    print(f"FOUND NONCE: {found_nonce} | RESULT: {status_text}")
    print(f"   {device_type} #{device_id}")

def show_error_simple(message):
    print(f"{message}")

def calc_seed(seed_input):
    # Create base (previous seed + real random)
    base_mix = (seed_input ^ secrets.randbelow(0xFFFFFFFF))
    # Mix through another hash round (avalanche effect)
    final_hash = hashlib.sha256(str(base_mix).encode()).hexdigest()
    # Return clean 32-bit number
    return int(final_hash[:8], 16)

def calc_nonce():
    return secrets.randbelow(0xFFFFFFFF)
 
def smart_trim(text, max_len=90):
    text = str(text)
    if len(text) <= max_len:
        return text
    side_len = (max_len - 3) // 2
    return f"{text[:side_len]}...{text[-side_len:]}"

def total_logger(msg, log_file_path):
    log_entry = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}\n"
    
    with open(log_file_path, "a", encoding="utf-8") as f:
        # Wait until another process releases the file and lock it
        portalocker.lock(f, portalocker.LOCK_EX) 
        try:
            f.write(log_entry)
            f.flush()
            os.fsync(f.fileno()) 
        finally:
            portalocker.unlock(f)

def encode_varint(n):
    if n < 253:
        return struct.pack("<B", n)
    elif n <= 0xFFFF:
        return b'\xfd' + struct.pack("<H", n)
    elif n <= 0xFFFFFFFF:
        return b'\xfe' + struct.pack("<L", n)
    else:
        return b'\xff' + struct.pack("<Q", n)

def send_result(args, header_data):
    found_nonce = header_data['found_nonce']
    header_76 = header_data['header_76']
    
    # Ensure header_76 is in bytes
    if isinstance(header_76, str): 
        header_76 = binascii.unhexlify(header_76)

    # Build final header (80 bytes)
    final_header = header_76 + struct.pack("<I", int(found_nonce) & 0xFFFFFFFF)
    header_data['final_header'] = final_header

    # Take already filtered data
    valid_txs_data = header_data.get('valid_txs_data', [])
    tx_cnt = len(valid_txs_data) + 1  # +1 for coinbase
    
    # Use VarInt for transaction count
    tx_cnt_bin = encode_varint(tx_cnt)

    # Build block: Header + TX_Count + Coinbase + Transactions
    block = final_header.hex() + \
            tx_cnt_bin.hex() + \
            header_data['cb'] + \
            "".join(valid_txs_data)

    header_data['block'] = block
    
    # Submit block
    res = call_rpc(args.cli, args.conf, args.log, "submitblock", [block])
    
    if True:
        # Result handling (Universal: for CLI and RPC)
        if res in [None, ""]: 
            status_text = "ACCEPTED"
        elif res == "inconclusive":
            status_text = "INCONCLUSIVE (Wait/Fork)"
        elif res == "duplicate":
            status_text = "DUPLICATE (Already found)"
        else:
            display_res = json.dumps(res) if isinstance(res, (dict, list)) else res
            status_text = f"REJECTED: {display_res}"

        show_block_found_simple(header_data, status_text)
        
        # Build list of lines for logs
        log_lines = []
        log_lines.append(f"\n{header_data['stats_key']} Mining data prepared:") 
        log_lines.append(f"    block_height: {header_data['height']}")
        log_lines.append(f"    extra_nonces: {header_data['extra_nonces']}")
        log_lines.append(f"    found_nonce: {header_data['found_nonce']}")
        log_lines.append(f"    target_list: {header_data['target_list']}")
        log_lines.append(f"    coinbase: {header_data['cb']}")
        log_lines.append(f"    merkle_root: {header_data['m_root'].hex()}")
        log_lines.append(f"    tpl: {header_data['tpl']}")
        log_lines.append(f"    header_76: {header_data['header_76'].hex()}")
        log_lines.append(f"    full_input: {header_data['full_input'].hex()}")
        log_lines.append(f"    final_header: {header_data['final_header'].hex()}")
        log_lines.append(f"    block: {header_data['block']}")
        log_lines.append(f"    Block submitted: [{status_text}]")
        
        final_message = "\n".join(log_lines) + "\n" 
        total_logger(f"{final_message}", args.log)
  
        return True
    return False

def calc_header_data(args, tpl, addr_hash, stats_key):

    def get_merkle_root(tx_hashes_bin, log_file):
        try:
            if not tx_hashes_bin:
                total_logger("DEBUG MERKLE ERROR: tx_hashes_bin is empty!", log_file)
                return None     
            hashes = list(tx_hashes_bin)
            while len(hashes) > 1:
                if len(hashes) % 2 != 0: hashes.append(hashes[-1])    
                new_hashes = []
                for i in range(0, len(hashes), 2):
                    combined = hashes[i] + hashes[i+1]
                    h = hashlib.sha256(hashlib.sha256(combined).digest()).digest()
                    new_hashes.append(h)
                hashes = new_hashes
            return hashes[0]
        except Exception as e:
            error_details = traceback.format_exc()
            total_logger(f"CRITICAL MERKLE ERROR:\n{error_details}", log_file)
            show_error_simple(f"[!] Merkle Error: {e.__class__.__name__}")
            time.sleep(5)
            return None
        
    def prepare_block_header(args, tpl, addr_hash, extra_nonces_list=None):
        height = tpl.get('height', 0)

        h_bin = struct.pack("<I", height).rstrip(b'\x00')
        
        if extra_nonces_list is not None and len(extra_nonces_list) > 0:
            extra_nonces_bytes = b''
            for nonce_int in extra_nonces_list:
                extra_nonces_bytes += struct.pack("<I", nonce_int & 0xFFFFFFFF)
            
            script_sig = (
                bytes([len(h_bin)]) + h_bin + 
                b'\x01\x01' +
                extra_nonces_bytes
            ).hex()
        else:
            script_sig = (bytes([len(h_bin)]) + h_bin + b'\x01\x01').hex()
            
        # Witness Commitment and Filtering
        witness_commitment = tpl.get('default_witness_commitment', '')
        
        # Filter transactions by LockTime (ignore future ones)
        valid_transactions = []
        for tx in tpl.get('transactions', []):
            if tx.get('locktime', 0) <= height:
                valid_transactions.append(tx)

        # Determine number of outputs (vout) in Coinbase
        if witness_commitment:
            output_count = "02"
            out1 = binascii.hexlify(struct.pack("<Q", tpl['coinbasevalue'])).decode() + "1976a914" + addr_hash + "88ac"
            out2 = "0000000000000000" + "26" + witness_commitment
            outputs = out1 + out2
        else:
            output_count = "01"
            outputs = binascii.hexlify(struct.pack("<Q", tpl['coinbasevalue'])).decode() + "1976a914" + addr_hash + "88ac"

        # Build final Coinbase
        cb = ("0100000001" + "00"*32 + "ffffffff" + 
              format(len(script_sig)//2, '02x') + script_sig + "ffffffff" +
              output_count + outputs + "00000000")

        cb_bin = binascii.unhexlify(cb)
        cb_hash = hashlib.sha256(hashlib.sha256(cb_bin).digest()).digest()
        
        # Always use txid instead of hash for SegWit (fix for bad-txnmrklroot)
        tx_hashes = [cb_hash]
        for tx in valid_transactions:
            t_id = tx.get('txid', tx['hash'])
            tx_hashes.append(binascii.unhexlify(t_id)[::-1]) 

        m_root = get_merkle_root(tx_hashes, args.log)
 
        if m_root is None: 
            return None
        
        header_76 = struct.pack("<I", tpl['version']) + binascii.unhexlify(tpl['previousblockhash'])[::-1] + \
                    m_root + struct.pack("<I", tpl['curtime']) + binascii.unhexlify(tpl['bits'])[::-1]
        
        full_input = header_76 + b'\x00\x00\x00\x00'
        target_list = [int(tpl['target'][i:i+8], 16) for i in range(0, 64, 8)][::-1]

        return {
            'stats_key': stats_key,
            'header_76': header_76,
            'full_input': full_input,
            'target_list': target_list,
            'cb': cb,
            'height': height,
            'tpl': tpl,
            'm_root': m_root,
            'valid_txs_data': [tx['data'] for tx in valid_transactions]
        }


    # Random count 1-12
    count = secrets.randbelow(12) + 1
    # Unique random extra nonces
    extra_nonces = list({secrets.randbelow(0xFFFFFFFF) for _ in range(count * 2)})[:count]
    while True:
        header_data = prepare_block_header(args,tpl,addr_hash,extra_nonces_list=extra_nonces)
        if header_data is not None:
            break
        count = secrets.randbelow(12) + 1
        extra_nonces = list({secrets.randbelow(0xFFFFFFFF) for _ in range(count * 2)})[:count]
        time.sleep(1)

    # Prepare parameters for DLL
    h_u32 = (ctypes.c_uint32 * 20)(*struct.unpack("<20I", header_data['full_input']))
    t_ptr = (ctypes.c_uint32 * 8)(*header_data['target_list'])
    header_data['extra_nonces'] = extra_nonces

    return header_data, h_u32, t_ptr 

def validate_args(args):
    if not os.path.exists(args.cli):
        raise FileNotFoundError(f"CLI not found: {args.cli}")
    if not os.path.exists(args.conf):
        raise FileNotFoundError(f"Config not found: {args.conf}")

def report_to_server(args, stats_dict, share_event, stop_event):
    base_name = socket.gethostname()
    worker_name = f"{base_name}-{args.name}" if args.name else base_name
    
    stop_event.wait(timeout=10)
    first_run = True
    
    while True:
        if not first_run:
            # Wait 180 sec for share OR stop signal
            share_event.wait(timeout=180)
        else:
            first_run = False
            
        # First check if share was found
        is_share_found = share_event.is_set()
        
        current_stats = dict(stats_dict)
        
        # Correct sum calculation for both formats
        total_mhs = 0.0
        gpu_details = {}
        
        for key, value in current_stats.items():
            if isinstance(value, dict):
                speed = value.get("speed", 0.0)
                if isinstance(speed, (int, float)):
                    total_mhs += speed
                gpu_details[key] = value
            elif isinstance(value, (int, float)):
                total_mhs += value
                gpu_details[key] = {"speed": value}
        
        total_mhs = round(total_mhs, 2)
        
        # Make a copy of command line parameters
        config_data = vars(args).copy() 
        
        # REMOVE PASSWORD
        if 'api_pass' in config_data:
            del config_data['api_pass']
            
        payload = {
            "pass": args.api_pass,
            "worker": worker_name,
            "hashrate": total_mhs,
            "details": json.dumps(gpu_details, ensure_ascii=False),
            "config": json.dumps(config_data),
            "event": "Share Found" if is_share_found else "Mining",
            "address": args.address,
            "timestamp": int(time.time())
        }
        
        try:
            requests.post(args.remote_url, data=payload, timeout=(60, 120))
        except Exception as e:
            pass
            
        if is_share_found:
            share_event.clear()
        
        if stop_event.is_set():
            break
            
        if stop_event.wait(timeout=2):
            break



def init_rpc_auth(conf_path, log_file):
    """Called once at miner start"""
    global RPC_CONFIG
    try:
        if os.path.exists(conf_path):
            with open(conf_path, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith('#') or '=' not in line:
                        continue
                    k, v = [x.strip() for x in line.split('=', 1)]
                    if k == 'rpcuser': RPC_CONFIG['user'] = v
                    elif k == 'rpcpassword': RPC_CONFIG['pass'] = v
                    elif k == 'rpcport': RPC_CONFIG['port'] = v
            RPC_CONFIG['initialized'] = True
    except Exception as e:
        total_logger(f"CRITICAL CONFIG READ ERROR: {e}", log_file)

def call_rpc(cli_path, conf_path, log_file, method, params=[]):
    global RPC_CONFIG
    
    # Auto-initialize if init_rpc_auth was skipped
    if not RPC_CONFIG['initialized']:
        init_rpc_auth(conf_path, log_file)

    rpc_url = f"http://127.0.0.1:{RPC_CONFIG['port']}"
    payload = {
        "jsonrpc": "1.0",
        "id": "mining-utils",
        "method": method,
        "params": params
    }

    if (method=="submitblock"):
        request_log = (
            f"--- RPC REQUEST ---\n"
            f"Method: {method}\n"
            f"Payload: {json.dumps(payload)}\n"
            f"-------------------"
        )
        total_logger(request_log, log_file)

    try:
        tm = 1000
        res = requests.post(
            rpc_url, 
            json=payload, 
            auth=(RPC_CONFIG['user'], RPC_CONFIG['pass']), 
            timeout=tm
        )

        
        if (method=="submitblock"):
            response_data = res.text.strip()
            status_log = (
                f"--- RPC RESPONSE ---\n"
                f"Method: {method}\n"
                f"Status Code: {res.status_code}\n"
                f"Response Body: {response_data}\n"
                f"--------------------"
            )
            total_logger(status_log, log_file)

        if res.status_code != 200:
            show_error_simple(f"[!] RPC Error: {method} failed (Status: {res.status_code})")
            return None

        return res.json().get('result')

    except Exception as e:
        error_details = traceback.format_exc()
        show_error_simple(f"\n[!] Critical RPC Error: {method}")
        total_logger(f"CRITICAL RPC EXCEPTION [{method}]:\n{error_details}", log_file)
        return None


def call_cli(cli_path, conf_path, log_file, method, params=[]):
    cmd = [cli_path, f"-conf={conf_path}", method]
    for p in params:
        cmd.append(json.dumps(p) if isinstance(p, (dict, list)) else str(p))

    try:
        tm = 600 if method == "submitblock" else 150
        
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=tm)
        
        if res.returncode != 0:
            err_msg = res.stderr.strip()
            show_error_simple(f"[!] RPC Error: {method} failed")
            total_logger(f"RPC ERROR [{method}]:\nCommand: {cmd}\nError: {err_msg}", log_file)
            return None

        output = res.stdout.strip()
        if output.startswith(("{", "[")):
            return json.loads(output)
        return output

    except subprocess.TimeoutExpired:
        show_error_simple(f"\n[!] Timeout: {method} (> {tm}s)")
        total_logger(f"TIMEOUT ERROR:\nMethod: {method}\nCommand: {cmd}", log_file)
        return None
        
    except Exception as e:
        show_error_simple(f"\n[!] Critical RPC Error: {method}")
        error_details = traceback.format_exc()
        total_logger(f"CRITICAL ERROR [{method}]:\n{error_details}\nCommand: {cmd}", log_file)
        return None
