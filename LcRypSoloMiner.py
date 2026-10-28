# ==============================================================================
# LcRyp Solo Miner v2.2
# ==============================================================================

import signal
import atexit
import psutil
import subprocess
import sys
from importlib import metadata

def install_missing_packages():
    required = {'requests', 'portalocker', 'psutil', 'pywin32'}
    try:
        installed = {dist.metadata['Name'].lower() for dist in metadata.distributions()}
    except Exception:
        installed = set()
    missing = required - installed
    if missing:
        print(f"[*] First run: installing modules {missing}...")
        try:
            subprocess.check_call([
                sys.executable, "-m", "pip", "install", 
                *missing, 
                "--disable-pip-version-check", 
                "--quiet"
            ])
            print("[+] Done. Starting miner...")
        except Exception as e:
            print(f"[-] Failed to install modules: {e}")
            sys.exit(1)

install_missing_packages()

import json, ctypes, struct, time, binascii, hashlib, multiprocessing, argparse, sys, os, socket, threading, requests, traceback, secrets, psutil, inspect, re, portalocker
from multiprocessing import Lock, Process
from mining_utils import * 
from mining_workers import *

def run(args):

    def cleanup_all():
        # Use global to check if gpu_lib was created
        if 'gpu_lib' in globals() and gpu_lib is not None:
            try:
                if hasattr(gpu_lib, 'cleanup_mining_drivers'):
                    gpu_lib.cleanup_mining_drivers()
                
                # Kill processes only if the function is imported
                if 'kill_gpu_processes' in globals():
                    kill_gpu_processes()
            except Exception as e:
                print(f"[-] Error during GPU cleanup: {e}")
        else:
            # If gpu_lib is not in globals, we are in CPU-only mode
            pass

    # Register cleanup on any exit
    atexit.register(cleanup_all)

    def signal_handler(signum, frame):
        print(f"\n[!] Received signal {signum}. Shutting down...")
        cleanup_all()
        sys.exit(0)

    # Add signal handlers
    signal.signal(signal.SIGINT, signal_handler)  # Ctrl+C
    signal.signal(signal.SIGTERM, signal_handler) # System termination

    # Additional cleanup on exit
    def cleanup_at_exit():
        if 'gpu_lib' in locals():
            try:
                gpu_lib.cleanup_mining_drivers()
            except:
                pass
    
    atexit.register(cleanup_at_exit)

    if os.path.exists(args.log):
        try:
            with open(args.log, 'w', encoding='utf-8') as f:
                f.write('')
        except Exception as e:
            pass   

    share_event = multiprocessing.Event()
    stop_event = multiprocessing.Event()
    
    # Parse CPU cores (indexing starts from 0)
    cpu_ids = [int(x.strip()) for x in str(args.cpus).split(",") if x.strip()]
    
    # Parse CPU batch parameters
    cpu_batch_values = parse_comma_list(args.cpu_batch, len(cpu_ids) if cpu_ids else 1, 5000)
    
    # Parse GPU indices for each type separately (indexing starts from 0)
    gpu_ids_integrated = [int(x.strip()) for x in str(args.gpus_integrated).split(",") if x.strip()]
    gpu_ids_green = [int(x.strip()) for x in str(args.gpus_green).split(",") if x.strip()]
    gpu_ids_red = [int(x.strip()) for x in str(args.gpus_red).split(",") if x.strip()]
    
    cpu_total_workers = len(cpu_ids) 
    gpu_total_workers = len(gpu_ids_integrated) + len(gpu_ids_green) + len(gpu_ids_red)
    
    # Parse parameters for each GPU type
    gpu_batch_integrated = parse_comma_list(args.gpu_batch_integrated, len(gpu_ids_integrated) if gpu_ids_integrated else 1, 1)
    gpu_bpg_integrated = parse_comma_list(args.gpu_bpg_integrated, len(gpu_ids_integrated) if gpu_ids_integrated else 1, 4096)
    gpu_tpb_integrated = parse_comma_list(args.gpu_tpb_integrated, len(gpu_ids_integrated) if gpu_ids_integrated else 1, 256)
    
    gpu_batch_green = parse_comma_list(args.gpu_batch_green, len(gpu_ids_green) if gpu_ids_green else 1, 100)
    gpu_bpg_green = parse_comma_list(args.gpu_bpg_green, len(gpu_ids_green) if gpu_ids_green else 1, 4096)
    gpu_tpb_green = parse_comma_list(args.gpu_tpb_green, len(gpu_ids_green) if gpu_ids_green else 1, 256)
    
    gpu_batch_red = parse_comma_list(args.gpu_batch_red, len(gpu_ids_red) if gpu_ids_red else 1, 100)
    gpu_bpg_red = parse_comma_list(args.gpu_bpg_red, len(gpu_ids_red) if gpu_ids_red else 1, 4096)
    gpu_tpb_red = parse_comma_list(args.gpu_tpb_red, len(gpu_ids_red) if gpu_ids_red else 1, 256)
    
    # Check GPU DLL loading
    if args.gpu_dll and os.path.exists(args.gpu_dll):
        try:
            gpu_lib = ctypes.CDLL(args.gpu_dll)
            gpu_lib.count_all_gpus.restype = ctypes.c_int
            gpu_lib.get_gpu_name.restype = ctypes.c_char_p
            gpu_lib.get_gpu_name.argtypes = [ctypes.c_int]
            
            # Get GPU count for each type
            detected_integrated = gpu_lib.count_gpus_by_type(0)
            detected_green = gpu_lib.count_gpus_by_type(1)
            detected_red = gpu_lib.count_gpus_by_type(2)
            
            msg = f"GPU Dispatcher loaded. Found: INT:{detected_integrated}, NV:{detected_green}, AMD:{detected_red}"
            print(msg)
            total_logger(msg, args.log)
            
            # Check all GPUs
            all_gpu_ids = []
            if gpu_ids_integrated:
                for i in gpu_ids_integrated:
                    if i < detected_integrated:
                        try:
                            gpu_id = gpu_lib.get_gpu_id(0, i)
                            name_bytes = gpu_lib.get_gpu_name(gpu_id)
                            if name_bytes:
                                name = name_bytes.decode('utf-8', errors='ignore')
                                device_msg = f"   └─ Integrated GPU [{i}]: {name}"
                                print(device_msg)
                                total_logger(device_msg, args.log)
                                all_gpu_ids.append((0, i))
                        except:
                            pass
            
            if gpu_ids_green:
                for i in gpu_ids_green:
                    if i < detected_green:
                        try:
                            gpu_id = gpu_lib.get_gpu_id(1, i)
                            name_bytes = gpu_lib.get_gpu_name(gpu_id)
                            if name_bytes:
                                name = name_bytes.decode('utf-8', errors='ignore')
                                device_msg = f"   └─ Green GPU [{i}]: {name}"
                                print(device_msg)
                                total_logger(device_msg, args.log)
                                all_gpu_ids.append((1, i))
                        except:
                            pass
            
            if gpu_ids_red:
                for i in gpu_ids_red:
                    if i < detected_red:
                        try:
                            gpu_id = gpu_lib.get_gpu_id(2, i)
                            name_bytes = gpu_lib.get_gpu_name(gpu_id)
                            if name_bytes:
                                name = name_bytes.decode('utf-8', errors='ignore')
                                device_msg = f"   └─ Red GPU [{i}]: {name}"
                                print(device_msg)
                                total_logger(device_msg, args.log)
                                all_gpu_ids.append((2, i))
                        except:
                            pass
                            
        except Exception as e:
            error_details = traceback.format_exc()
            total_logger(f"GPU DLL Initialization Error:\n{error_details}", args.log)
            error_msg = f"GPU DLL Error: {e.__class__.__name__}: {str(e)}"
            show_error_simple(f"[!] {error_msg}")
            time.sleep(5)
            args.gpu_dll = None
    else:
        args.gpu_dll = None
    
    last_height = 0
    alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    num = 0
    for char in args.address: 
        num = num * 58 + alphabet.index(char)
    addr_hash = num.to_bytes(25, byteorder='big')[1:-4].hex()
    total_logger(f"\nSOLO TURBO ONLINE | ADDR: {args.address}\n", args.log)
    print(f"\nSOLO TURBO ONLINE | ADDR: {args.address}\n")

    manager = multiprocessing.Manager()
    stats_dict = manager.dict()
    tpl_lock = manager.Lock()

    while True:
        try:
            tpl = call_rpc(args.cli, args.conf, args.log, "getblocktemplate", [{"rules": ["segwit"]}])

            if not tpl or not isinstance(tpl, dict) or 'height' not in tpl:
                time.sleep(1)
                continue
           
            height = tpl['height']

            # Clear events for new block
            share_event.clear()
            stop_event.clear()

            # Pass manager to start_workers with new parameters
            result_queue, stats_dict, procs, shared_tpl = start_workers(
                args, 
                tpl, 
                addr_hash, 
                cpu_ids, 
                cpu_batch_values,
                gpu_ids_integrated,
                gpu_batch_integrated,
                gpu_bpg_integrated,
                gpu_tpb_integrated,
                gpu_ids_green,
                gpu_batch_green,
                gpu_bpg_green,
                gpu_tpb_green,
                gpu_ids_red,
                gpu_batch_red,
                gpu_bpg_red,
                gpu_tpb_red,
                share_event, 
                stop_event, 
                manager, 
                stats_dict, 
                tpl_lock
            )

            last_nonce_update = time.time()
            last_display_update = time.time()

            try:
                while result_queue.empty():
                    current_time = time.time()          
                    # Update statistics every 10 sec
                    if current_time - last_display_update >= 10:
                        if len(stats_dict) > 0:
                            update_stats_simple(stats_dict, height)
                            last_display_update = current_time
                    # Check template updates every 10 sec
                    if current_time - last_nonce_update < 10:
                        time.sleep(0.5)
                        continue
                    
                    last_nonce_update = current_time
                    new_tpl = call_rpc(args.cli, args.conf, args.log, "getblocktemplate", [{"rules": ["segwit"]}])
                    
                    if new_tpl and isinstance(new_tpl, dict):
                        if new_tpl.get('height') != height:
                            total_logger(f"New block {new_tpl.get('height')} detected. Restarting...", args.log)
                            height = new_tpl['height']
                            stop_event.set()
                            time.sleep(0.5)
                            break
                        else:
                            with tpl_lock:
                                shared_tpl.update(new_tpl) 

                if not result_queue.empty():
                    header_data = result_queue.get()
                    send_result(args, header_data)
            finally:
                # Guaranteed process termination
                stop_event.set()
                time.sleep(0.1)
                
                for p in procs:
                    if p.is_alive():
                        p.terminate()
                        p.join(timeout=1.0)
                        
                for p in procs:
                    if p.is_alive():
                        p.kill()
                        p.join(timeout=0.5)
        except Exception as e:
            error_details = traceback.format_exc()
            total_logger(f"CRITICAL RUN ERROR:\n{error_details}", args.log)
            show_error_simple(f"\n[!] Error:\n{error_details}")
            time.sleep(5)

def parse_comma_list(param, expected_count=None, default_value=None):
    if not param:
        return []
    
    values = [v.strip() for v in param.split(',') if v.strip()]
    
    result = []
    for v in values:
        try:
            result.append(int(v))
        except ValueError:
            try:
                result.append(float(v))
            except ValueError:
                result.append(v)
    
    if expected_count is not None and len(result) < expected_count:
        if result:
            last_value = result[-1]
            result.extend([last_value] * (expected_count - len(result)))
        else:
            result.extend([default_value] * expected_count)
    return result

def validate_args(args):
    if not args.address or len(args.address) < 10:
        raise ValueError("Invalid payout address")
    
    if not os.path.exists(args.cli):
        raise FileNotFoundError(f"Cannot find lcryp-cli.exe at: {args.cli}")
    
    if not os.path.exists(args.conf):
        raise FileNotFoundError(f"Cannot find conf file at: {args.conf}")
    
    if not os.path.exists(args.cpu_dll):
        raise FileNotFoundError(f"Cannot find CPU DLL at: {args.cpu_dll}")
    
    def validate_comma_list(param, name):
        if param:
            values = [v.strip() for v in param.split(',') if v.strip()]
            for v in values:
                if not v.isdigit():
                    raise ValueError(f"Invalid value in {name}: {param} - all values must be integers")
    
    validate_comma_list(args.gpus_integrated, "--gpus_integrated")
    validate_comma_list(args.gpus_green, "--gpus_green")
    validate_comma_list(args.gpus_red, "--gpus_red")
    validate_comma_list(args.cpus, "--cpus")

def parse_args():
    current_dir = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(add_help=False) 
    
    parser.add_argument("--help", "-h", action="store_true", help="Show this help message")
    parser.add_argument("--info", "-i", action="store_true", help="Show hardware info and exit")
    
    parser.add_argument("--address", type=str, default="1KSFJYLJMJ83zchYS8VjoFWvmzt4hQAq3i", help="Mining payout address")

    parser.add_argument("--cli", type=str, default="lcryp-cli.exe", help="Path to lcryp-cli.exe")
    parser.add_argument("--conf", type=str, default="lcryp.conf", help="Path to lcryp.conf")
    parser.add_argument("--log", type=str, default="debug.log", help="Log file name") 
    
    parser.add_argument("--cpu_dll", type=str, default="LcRypMinerCoreAVX2.dll", help="Path to LcRypMinerCoreAVX2.dll OR ANY OTHER YOUR dll") 
    parser.add_argument("--gpu_dll", type=str, default="GpuModuleBridge.dll", help="Path to GpuModuleBridge.dll") 

    parser.add_argument("--cpus", type=str, default="", help="List of CPU cores, e.g., '0,1,2,3'")
    parser.add_argument("--cpu_batch", type=str, default="5000", help="Batch List of CPU, e.g., '5000,5000,5000,5000'")
    
    parser.add_argument("--gpus_integrated", type=str, default="", help="List of Integrated GPU gpus, e.g., '0,1,2,3'")
    parser.add_argument("--gpu_batch_integrated", type=str, default="1", help="Batch List of Integrated GPU, e.g., '1,1,1,1'")
    parser.add_argument("--gpu_bpg_integrated", type=str, default="4096", help="Set List of bpg for Integrated GPU, e.g., '4096,4096,4096,4096'")
    parser.add_argument("--gpu_tpb_integrated", type=str, default="256", help="Set List of tpb for Integrated GPU, e.g., '256,256,256,256'")
    
    parser.add_argument("--gpus_green", type=str, default="", help="List of Green GPU gpus, e.g., '0,1,2,3'")
    parser.add_argument("--gpu_batch_green", type=str, default="100", help="Batch List of Green GPU, e.g., '100,100,100,100'")
    parser.add_argument("--gpu_bpg_green", type=str, default="4096", help="Set List of bpg for Green GPU, e.g., '4096,4096,4096,4096'")
    parser.add_argument("--gpu_tpb_green", type=str, default="256", help="Set List of tpb for Green GPU, e.g., '256,256,256,256'")
    
    parser.add_argument("--gpus_red", type=str, default="", help="List of Red GPU gpus, e.g., '0,1,2,3'")
    parser.add_argument("--gpu_batch_red", type=str, default="100", help="Batch List of Red GPU, e.g., '100,100,100,100'")
    parser.add_argument("--gpu_bpg_red", type=str, default="4096", help="Set List of bpg for Red GPU, e.g., '4096,4096,4096,4096'")
    parser.add_argument("--gpu_tpb_red", type=str, default="256", help="Set List of tpb for Red GPU, e.g., '256,256,256,256'")

    parser.add_argument("--remote_url", type=str, default="https://lcryp.com/worker_api.php", help="URL for statistics")
    parser.add_argument("--api_pass", type=str, default="12345678", help="API Password for monitor")
    parser.add_argument("--name", type=str, default="default", help="Additional name for the worker")

    args = parser.parse_args()
    init_rpc_auth(args.conf, args.log)
    if not args.help:
        if args.cli: args.cli = args.cli.strip('"\'')
        if args.conf: args.conf = args.conf.strip('"\'')
        if args.address: args.address = args.address.strip('"\'')
        if not os.path.dirname(args.log):
            args.log = os.path.join(current_dir, args.log).strip('"\'')
        if not os.path.dirname(args.cpu_dll):
            args.cpu_dll = os.path.join(current_dir, args.cpu_dll).strip('"\'')
        if not os.path.dirname(args.gpu_dll):
            args.gpu_dll = os.path.join(current_dir, args.gpu_dll).strip('"\'')
        if args.gpu_dll and not os.path.exists(args.gpu_dll):
            args.gpu_dll = None
        if not args.info:
            validate_args(args)
            args_dict = vars(args).copy()
            total_logger(f"FULL ARGS JSON: {json.dumps(args_dict, indent=2)}", args.log)
    return args

def print_help(): 
    help_text = """
LcRyp Solo Miner - Command Line Interface

BASIC COMMANDS:
  --help                        Show this help message
  --info                        Show hardware info and exit
  
  --address <addr>              Mining payout address
  
  --cli <path>                  Path to lcryp-cli.exe
  --conf <path>                 Path to lcryp.conf
  --log <file>                  Log file name
  
  --cpu_dll <path>              Path to LcRypMinerCoreAVX2.dll OR ANY OTHER YOUR dll
  --gpu_dll <path>              Path to GpuModuleBridge.dll
  
  --cpus <list>                 List of CPU cores, e.g., '0,1,2,3'
  --cpu_batch <list>            Batch sizes for CPU cores, e.g., '5000,5000,5000,5000'
  
  --gpus_integrated <list>      List of Integrated GPU indices, e.g., '0'
  --gpu_batch_integrated <list> Batch sizes for Integrated GPUs, e.g., '1'
  --gpu_bpg_integrated <list>   Blocks per grid for Integrated GPUs, e.g., '4096'
  --gpu_tpb_integrated <list>   Threads per block for Integrated GPUs, e.g., '256'
    
  --gpus_green <list>           List of Green (NVIDIA) GPU indices, e.g., '0,1,2,3'
  --gpu_batch_green <list>      Batch sizes for Green GPUs, e.g., '100,100,100,100'
  --gpu_bpg_green <list>        Blocks per grid for Green GPUs, e.g., '4096,4096,4096,4096'
  --gpu_tpb_green <list>        Threads per block for Green GPUs, e.g., '256,256,256,256'
  
  --gpus_red <list>             List of Red (AMD) GPU indices, e.g., '0,1,2,3'
  --gpu_batch_red <list>        Batch sizes for Red GPUs, e.g., '100,100,100,100'
  --gpu_bpg_red <list>          Blocks per grid for Red GPUs, e.g., '4096,4096,4096,4096'
  --gpu_tpb_red <list>          Threads per block for Red GPUs, e.g., '256,256,256,256'

  --remote_url                  URL for statistics (default: https://lcryp.com/worker_api.php)
  --api_pass                    API Password for monitor
  --name                        Additional name for the worker


RECOMMENDED lcryp.conf CONTENT:
  server=1
  rpctimeout=30
  rpcuser=your_username
  rpcpassword=your_password
  rpcport=4994
  printtoconsole=1
  listen=1
  dbcache=4000
  rest=1
  rpcworkqueue=100
  txindex=1
  addr=1
  txconfirmtarget=6
  peertimeout=60
  minrelaytxfee=0.00001
  blockmintxfee=0.00000001
  limitfreerelay=1000
  mempoolreplacement=1
  incrementalrelayfee=0.00000001
  maxmempool=100


USAGE EXAMPLES:
  python LcRypSoloMiner.py --help
  python LcRypSoloMiner.py --info
  
  # CPU mining only (8 cores)
  python LcRypSoloMiner.py --address "YOUR_ADDRESS" 
                          --cli "lcryp-cli.exe" 
                          --conf "lcryp.conf" 
                          --cpu_dll="LcRypMinerCoreAVX2.dll" 
                          --cpus "0,1,2,3,4,5,6,7"
                          --cpu_batch="5000"
                          --name "CPU_Worker"
  
  # Mixed mining
  python LcRypSoloMiner.py --address "YOUR_ADDRESS" 
                          --cli "lcryp-cli.exe" 
                          --conf "lcryp.conf" 
                          --cpu_dll="LcRypMinerCoreAVX2.dll" 
                          --gpu_dll="GpuModuleBridge.dll"
                          --cpus "0,2,4,6"
                          --gpus_green="0,1" 
                          --gpu_batch_green="100,150"
                          --name "Mixed_Miner"


IMPORTANT NOTES:
  • Use --info to see available hardware indices
  • Each GPU type has its own independent indexing starting from 0
  • Lists must have the same number of elements as devices selected
  • If list is shorter than devices, last value repeats for remaining devices

DEFAULT PARAMETERS:
  --address "1KSFJYLJMJ83zchYS8VjoFWvmzt4hQAq3i"
  --cli "lcryp-cli.exe"
  --conf "lcryp.conf"
  --log "debug.log" 
  --cpu_dll "LcRypMinerCoreAVX2.dll" 
  --gpu_dll "GpuModuleBridge.dll"
  --remote_url "https://lcryp.com/worker_api.php" 
  --api_pass "12345678" 
  --name "default" 
    """
    print(help_text)

if __name__ == "__main__":
    version = "v2.2"
    multiprocessing.freeze_support()
    parsed_args = parse_args()

    print(f"{'-'*80}")
    print(f"|{'LcRyp Solo Miner v2.2'.center(78)}|")
    print(f"{'-'*80}")

    if parsed_args.help:
        print_help()
        sys.exit()

    detected_integrated_gpus = 0
    detected_green_gpus = 0
    detected_red_gpus = 0
    total_gpus = 0
    
    if parsed_args.gpu_dll and os.path.exists(parsed_args.gpu_dll):
        try:
            gpu_lib = ctypes.CDLL(parsed_args.gpu_dll)
            
            detected_integrated_gpus = gpu_lib.count_gpus_by_type(0)
            detected_green_gpus = gpu_lib.count_gpus_by_type(1)
            detected_red_gpus = gpu_lib.count_gpus_by_type(2)
            
            total_gpus = detected_integrated_gpus + detected_green_gpus + detected_red_gpus
                
        except Exception as e:
            detected_integrated_gpus = detected_green_gpus = detected_red_gpus = 0
            total_gpus = 0

    if parsed_args.info:
        print(f"\n{'-'*80}")
        print(f"|{'HARDWARE INFORMATION'.center(78)}|")
        print(f"{'-'*80}")
        
        cpus = multiprocessing.cpu_count()
        print(f"CPU: Found {cpus} logical cpus.") 
        
        if parsed_args.cpu_dll:
            cpu_list = parse_comma_list(parsed_args.cpus)
            cpu_batch_list = parse_comma_list(parsed_args.cpu_batch, len(cpu_list) if cpu_list else 1, 5000)
            
            if cpu_list:
                print(f"      Configured CPUs: {','.join(map(str, cpu_list))}")
                print(f"      CPU batch sizes: {','.join(map(str, cpu_batch_list))}")
            else:
                print(f"      To use all CPUs: --cpus {','.join(map(str, range(cpus)))}")
                print(f"      Example: --cpu_batch {','.join(['5000']*cpus)}")
        
        if parsed_args.gpu_dll:
            gpu_lib.get_gpu_name.restype = ctypes.c_char_p
            
            print(f"\nGPU: Found via {os.path.basename(parsed_args.gpu_dll)}")
            
            if detected_integrated_gpus > 0:
                print(f"      Integrated GPUs: {detected_integrated_gpus} devices")
                for i in range(detected_integrated_gpus):
                    try:
                        gpu_id = gpu_lib.get_gpu_id(0, i)
                        gpu_name = gpu_lib.get_gpu_name(gpu_id).decode('utf-8', errors='ignore')
                        print(f"        [{i}] {gpu_name}")
                    except:
                        print(f"        [{i}] Unknown Integrated GPU")
                print(f"      To use all Integrated GPUs: --gpus_integrated {','.join(map(str, range(detected_integrated_gpus)))}")
            
            if detected_green_gpus > 0:
                print(f"      Green GPUs: {detected_green_gpus} devices")
                for i in range(detected_green_gpus):
                    try:
                        gpu_id = gpu_lib.get_gpu_id(1, i)
                        gpu_name = gpu_lib.get_gpu_name(gpu_id).decode('utf-8', errors='ignore')
                        print(f"        [{i}] {gpu_name}")
                    except:
                        print(f"        [{i}] Unknown NVIDIA GPU")
                print(f"      To use all Green GPUs: --gpus_green {','.join(map(str, range(detected_green_gpus)))}")
            
            if detected_red_gpus > 0:
                print(f"      Red GPUs: {detected_red_gpus} devices")
                for i in range(detected_red_gpus):
                    try:
                        gpu_id = gpu_lib.get_gpu_id(2, i)
                        gpu_name = gpu_lib.get_gpu_name(gpu_id).decode('utf-8', errors='ignore')
                        print(f"        [{i}] {gpu_name}")
                    except:
                        print(f"        [{i}] Unknown AMD GPU")
                print(f"      To use all Red GPUs: --gpus_red {','.join(map(str, range(detected_red_gpus)))}")
            
            if total_gpus == 0:
                print(f"GPU: No GPUs detected via DLL")
        else:
            print(f"\nGPU: GPU DLL not found.") 
        
        print(f"{'-'*80}\n")
        sys.exit(0)

    cpu_list = parse_comma_list(parsed_args.cpus)
    cpu_batch_list = parse_comma_list(parsed_args.cpu_batch, len(cpu_list) if cpu_list else 1, 5000)
    
    integrated_list = parse_comma_list(parsed_args.gpus_integrated)
    integrated_batch_list = parse_comma_list(parsed_args.gpu_batch_integrated, len(integrated_list) if integrated_list else 1, 1)
    integrated_bpg_list = parse_comma_list(parsed_args.gpu_bpg_integrated, len(integrated_list) if integrated_list else 1, 4096)
    integrated_tpb_list = parse_comma_list(parsed_args.gpu_tpb_integrated, len(integrated_list) if integrated_list else 1, 256)
    
    green_list = parse_comma_list(parsed_args.gpus_green)
    green_batch_list = parse_comma_list(parsed_args.gpu_batch_green, len(green_list) if green_list else 1, 100)
    green_bpg_list = parse_comma_list(parsed_args.gpu_bpg_green, len(green_list) if green_list else 1, 4096)
    green_tpb_list = parse_comma_list(parsed_args.gpu_tpb_green, len(green_list) if green_list else 1, 256)
    
    red_list = parse_comma_list(parsed_args.gpus_red)
    red_batch_list = parse_comma_list(parsed_args.gpu_batch_red, len(red_list) if red_list else 1, 100)
    red_bpg_list = parse_comma_list(parsed_args.gpu_bpg_red, len(red_list) if red_list else 1, 4096)
    red_tpb_list = parse_comma_list(parsed_args.gpu_tpb_red, len(red_list) if red_list else 1, 256)
    
    print(f"| ADDR:        {parsed_args.address:<63} |")
    
    cpu_info = f"{len(cpu_list)} core(s): {parsed_args.cpus}" if cpu_list else "None"
    print(f"| CPU:         {cpu_info:<63} |")
    
    gpu_info_parts = []
    if integrated_list:
        gpu_info_parts.append(f"INT:{len(integrated_list)}")
    if green_list:
        gpu_info_parts.append(f"NV:{len(green_list)}")
    if red_list:
        gpu_info_parts.append(f"AMD:{len(red_list)}")
    
    gpu_info = ", ".join(gpu_info_parts) if gpu_info_parts else "None"
    print(f"| GPU:         {gpu_info:<63} |")
    
    print(f"| WORKER:      {str(parsed_args.name):<63} |")
    print(f"{'-'*80}")
    print()
    
    print("Configured devices:")
    
    if cpu_list:
        print(f"  CPU cores:")
        for idx, cpu_idx in enumerate(cpu_list):
            batch = cpu_batch_list[idx] if idx < len(cpu_batch_list) else cpu_batch_list[-1] if cpu_batch_list else 5000
            print(f"    Core {cpu_idx}: batch={batch}")
    
    if integrated_list:
        print(f"  Integrated GPUs:")
        for idx, gpu_idx in enumerate(integrated_list):
            batch = integrated_batch_list[idx] if idx < len(integrated_batch_list) else integrated_batch_list[-1] if integrated_batch_list else 1
            bpg = integrated_bpg_list[idx] if idx < len(integrated_bpg_list) else integrated_bpg_list[-1] if integrated_bpg_list else 4096
            tpb = integrated_tpb_list[idx] if idx < len(integrated_tpb_list) else integrated_tpb_list[-1] if integrated_tpb_list else 256
            print(f"    GPU {gpu_idx}: batch={batch}, bpg={bpg}, tpb={tpb}")
    
    if green_list:
        print(f"  Green GPUs (NVIDIA):")
        for idx, gpu_idx in enumerate(green_list):
            batch = green_batch_list[idx] if idx < len(green_batch_list) else green_batch_list[-1] if green_batch_list else 100
            bpg = green_bpg_list[idx] if idx < len(green_bpg_list) else green_bpg_list[-1] if green_bpg_list else 4096
            tpb = green_tpb_list[idx] if idx < len(green_tpb_list) else green_tpb_list[-1] if green_tpb_list else 256
            print(f"    GPU {gpu_idx}: batch={batch}, bpg={bpg}, tpb={tpb}")
    
    if red_list:
        print(f"  Red GPUs (AMD):")
        for idx, gpu_idx in enumerate(red_list):
            batch = red_batch_list[idx] if idx < len(red_batch_list) else red_batch_list[-1] if red_batch_list else 100
            bpg = red_bpg_list[idx] if idx < len(red_bpg_list) else red_bpg_list[-1] if red_bpg_list else 4096
            tpb = red_tpb_list[idx] if idx < len(red_tpb_list) else red_tpb_list[-1] if red_tpb_list else 256
            print(f"    GPU {gpu_idx}: batch={batch}, bpg={bpg}, tpb={tpb}")
    
    if not (cpu_list or integrated_list or green_list or red_list):
        print("  No devices configured for mining")
    
    print()

    while True:
        try:
            run(parsed_args)

        except KeyboardInterrupt:
            print(f"\n[!] Graceful shutdown...")
            if 'gpu_lib' in locals() and hasattr(gpu_lib, 'cleanup_mining_drivers'):
                try:
                    gpu_lib.cleanup_mining_drivers()
                except:
                    pass
            sys.exit(0)
        except Exception as e:
            short_err = f"{e.__class__.__name__}: {str(e)}"[:100]
            print(f"\n[!!!] RESTARTING MONITORING: {short_err}")
            error_details = traceback.format_exc()
            total_logger(f"Monitoring restart error:\n{error_details}", parsed_args.log)
            time.sleep(5)
