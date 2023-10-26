## scAtteR client

The client component of the `scAtteR` system as described in the paper, "Characterizing Distributed Mobile Augmented Reality Applications at the Edge".

### Dependencies

- `Python`
  - `OpenCV`

### Running

To run the client, the following command can be used:

```sh
python client.py
```

The client reads the video file `input.mp4` by default and transmits the frames of the video file to the `primary` service of the `scAtteR` pipeline. The details of this service are specified in `client.py` in the variables `server_ip` and `server_port`. 

The client loops infinitely with reading, packaging, and sending frame data to the server.  