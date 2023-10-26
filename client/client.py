from http import client
import sys
import getopt

import random
import string
import time

import socket

import mimetypes
import cv2

from threading import Thread

def print_json(client_id, frame_no, message):
    print(
        f'{{\\"service_name\\": \\"client\\", \\"id\\": \\"{client_id}\\", \\"frame_no\\": \\"{frame_no}\\", \\"timestamp\\": \\"{time.time_ns()/1000/1000}\\", \\"message\\": \\"{message}\\"}}'
    )

def listen_data(sock):
    while True:
        data, addr = sock.recvfrom(60000)  # buffer size is 1024 bytes
        # print(data, addr)

        client_id = data[0:4].decode("utf-8")
        frame_id = int.from_bytes(data[4:8], "little")
        results_size = int.from_bytes(data[12:16], "little")

        print_json(
            client_id,
            frame_id,
            f"Received results for Frame {frame_id} that has a size of {results_size} marker(s)",
        )

def send_data(client_id, frame_type, frame_no, frame_buffer, sock, main_ip, main_port):
    # preparing the packet to be sent
    frame_bytes = ""
    if len(frame_buffer) == 0:
        frame_bytes = (0).to_bytes(4, "big")
    else:
        frame_bytes = frame_buffer.tobytes()
    frame_len = len(frame_bytes)

    payload_size = frame_len + 44
    frame_array = bytearray(payload_size)

    frame_array[0:4] = bytearray(client_id, "utf-8")  # client_id
    frame_array[4:8] = (frame_no).to_bytes(4, "little")  # frame_id
    frame_array[8:12] = (frame_type).to_bytes(4, "little")  # frame_type
    frame_array[12:16] = frame_len.to_bytes(4, "little")  # frame_len
    frame_array[40:44] = (0).to_bytes(4, "big")  # sift_len
    frame_array[44 : frame_len + 44] = frame_bytes  # frame_data

    if (frame_type == 2) & (frame_len < 2000):
        return
    sock.sendto(frame_array, (main_ip, int(main_port)))

    print_json(
        client_id,
        frame_no,
        f"Sent Frame {frame_no} of total payload length {payload_size} to main service for processing",
    )


def main():
    # hardcoding the server IP and port, and the input file
    server_ip = "192.168.1.102"
    server_port = 50501
    input_file = "input.mp4"

    if server_ip and server_port and input_file:
        client_id = "".join(random.choices(string.ascii_letters + string.digits, k=4))
        print_json(client_id, 0, f"Client created and assigned ID of {client_id}")
        print_json(
            client_id, 0, f"Details of main service provided: {server_ip}:{server_port}"
        )

        # Preparing UDP socket
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("", 0))
        client_assigned_port = sock.getsockname()[1]
        print_json(client_id, 0, f"Port {client_assigned_port} is bound to the client")

        # create separate thread to listen for messages
        thread = Thread(target=listen_data, args=(sock,))

        # start the threads
        thread.start()

        # Loading file with the name provided by the user
        mimetypes.init()
        mimestart = mimetypes.guess_type(input_file)[0]
        if mimestart != None:
            mimestart = mimestart.split("/")[0]

            if mimestart in ["audio", "video", "image"]:
                # Check if video or frame being provided and adjust accordingly
                print_json(client_id, 0, f"Provided file is of filetype: {mimestart}")

                frame_count = 1  # frames are not zero-indexed

                while True:
                    # Loading data to be sent
                    if mimestart == "video":
                        cap = cv2.VideoCapture(input_file)
                        fps = cap.get(cv2.CAP_PROP_FPS)
                        print_json(client_id, 0, f"Sending video at {fps} FPS")

                        while True:
                            now = time.time()
                            ret, frame = cap.read()
                            if not ret:
                                break  # break if no next frame

                            # preprocess the frame by greyscaling and resizing the resolution
                            scale_percent = 60  # percent of original size
                            width = 480
                            height = 270
                            dim = (width, height)

                            time_start = time.time_ns() / 1000 / 1000
                            frame_grey = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                            frame_resized = cv2.resize(
                                frame_grey, dim, interpolation=cv2.INTER_AREA
                            )
                            encoding_params = [int(cv2.IMWRITE_JPEG_QUALITY), 90]
                            retval, frame_buffer = cv2.imencode(
                                ".jpg", frame_resized, encoding_params
                            )
                            time_end = time.time_ns() / 1000 / 1000
                            print_json(
                                client_id,
                                frame_count,
                                f"Preprocessing of Frame {frame_count} took {time_end-time_start} ms",
                            )

                            # if frame_count < 2:
                            send_data(
                                client_id,
                                1,
                                frame_count,
                                frame_buffer,
                                sock,
                                server_ip,
                                server_port,
                            )
                                # exit()

                            frame_count += 1

                            timeDiff = time.time() - now
                            if timeDiff < 1.0 / (fps):
                                time.sleep(1.0 / (fps) - timeDiff)

                    elif mimestart == "image":
                        pass


if __name__ == "__main__":
    # main(sys.argv[1:])
    main()
