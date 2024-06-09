# from scipy.io import wavfile
# import numpy as np

# with open("i2s.raw", "rb") as inp_f:
#     data = inp_f.read()
#     data = np.frombuffer(data, dtype=np.int16)
#     wavfile.write("sound.wav", 32000, data)

import sys
import wave

channels = 2 if len(sys.argv) < 2 else int(sys.argv[1])
rate = 16000 if len(sys.argv) < 5 else int(sys.argv[4])
    
with open('i2s.raw', 'rb') as raw:
    wav = wave.open('playback.wav', 'wb')
    wav.setframerate(rate)
    wav.setsampwidth(2)
    wav.setnchannels(1)
    wav.writeframes(raw.read())
    wav.close()