import time
import copy
import threading
import queue
import os, sys

project_root = os.path.dirname(os.path.abspath(__file__))
src_path = os.path.join(project_root, "src")
if src_path not in sys.path:
    sys.path.insert(0, src_path)

from datetime import datetime

from utils.logger import get_logger
from utils.config_parser import ConfigParser
from utils.images import ImageRemove

from server_adaptor.grpc_adaptor import gRPCAdaptor
from server_adaptor.redis_adaptor import RedisAdaptor
from server_adaptor.volt_adaptor import VoltDBAdaptor
from server_adaptor.sqlite_adaptor import SqliteAdaptor

from data.merger import Merger
from data.lp_detector import LpDetector
from data.receiver import DataReceiver
from data.sender import DataSender
from data.constants import DataType as dt

class Main:
    def __init__(self):
        self.logger = get_logger("main")
        self.config = ConfigParser.get()
        
        # Queues Between Receivers, Sender, Processors
        self.to_server_q = queue.Queue()
        self.to_merge_q_2k = None
        self.to_merge_q_4k = None
        self.to_ocr_q = None

        # Set Up Processors According To The Config
        if self.config["merge"]["enabled"]:
            self.to_merge_q_2k = queue.Queue()
            self.to_merge_q_4k = queue.Queue()
            self.merger = Merger(self.to_server_q, self.to_merge_q_2k, self.to_merge_q_4k)
        if self.config["OCR"]["enabled"]:
            self.to_ocr_q = queue.Queue()
            self.lp_detector = LpDetector(self.to_server_q, self.to_ocr_q)
            
        # Build Data Receivers 
        self.receivers = []
        for ch in self.config["redis_rcv"]:
            adaptor = RedisAdaptor(ch)
            to_merge_q = None
            to_ocr_q = self.to_ocr_q
            if ch["label"] == dt.VEHICLE_2K:
                to_merge_q = self.to_merge_q_2k 
            elif ch["label"] == dt.VEHICLE_4K:
                to_merge_q = self.to_merge_q_4k
            receiver = DataReceiver(self.to_server_q, adaptor, to_merge_q, to_ocr_q)
            self.receivers.append(receiver)
            
        # Build Server Adaptors
        self.servers = []
        for s in self.config["servers"]:
            if s["type"] == "grpc":
                adaptor = gRPCAdaptor(s)
            elif s["type"] == "volt":
                adaptor = VoltDBAdaptor(s)
            elif s["type"] == "redis":
                adaptor = RedisAdaptor(s)
            elif s["type"] == "sqlite":
                adaptor = SqliteAdaptor(s)
            elif s["type"] == "manual":
                DataSender.set_camera_id(s["cam_id"])
            else:
                self.logger.critical("Unsupported Server Type!", extra={"error": s["type"]})
                continue
            self.servers.append(adaptor)
                        
        # Build SQLite Adaptor
        self.sqlite = SqliteAdaptor(self.config["sqlite"])
        self.sqlite.connect()
        
        # Build Data Sender
        self.data_sender = DataSender(self.to_server_q, self.servers, self.sqlite)

    def start(self):
        """thread 실행 위치"""
        self.running_threads = []
        
        # Run Receiver Threads
        for data_receiver in self.receivers:
            receiver_thread = threading.Thread(target=data_receiver.main_loop, name=f"{data_receiver.label} RCV", daemon=True)
            receiver_thread.start()
            self.running_threads.append(receiver_thread)
            
        # Run Sender Thread
        sender_thread = threading.Thread(target=self.data_sender.main_loop, name="SND", daemon=True)
        sender_thread.start()
        self.running_threads.append(sender_thread)
        
        # Run Failed Data Sender Thread
        failed_sender_thread = threading.Thread(target=self.data_sender.failed_data_sender.main_loop, name="SND-RETRY", daemon=True)
        failed_sender_thread.start()
        self.running_threads.append(failed_sender_thread)
        
        # Run Merge Thread
        if self.config["merge"]["enabled"]:
            merger_thread = threading.Thread(target=self.merger.main_loop, name="MRG", daemon=True)
            merger_thread.start()
            self.running_threads.append(merger_thread)
            
        # Run OCR Thread
        if self.config["OCR"]["enabled"]:
            ocr_thread = threading.Thread(target=self.lp_detector.main_loop, name="OCR", daemon=True)
            ocr_thread.start()
            self.running_threads.append(ocr_thread)

if __name__ == "__main__":
    """Main 실행 위치"""
    main = Main()
    main.start()
    while True:
        ImageRemove.cleanup()
        time.sleep(30)
