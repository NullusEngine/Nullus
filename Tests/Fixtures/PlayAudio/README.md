These three mono 48 kHz fixtures contain a generated 100 ms 440 Hz tone at low
volume. They are original test data, generated with Python's wave module and
SoundFile 0.13.1 (libsndfile), and contain no third-party recording.

WAV is signed 16-bit PCM. MP3 and OGG exercise the vendored miniaudio decoders.
No encoder or network download is required to build or run the tests.
