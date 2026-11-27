# Helper tools that run next to the server and send suggestions through /ext:
#   mb-match     compositions and arrangements from MusicBrainz
#   audio-match  arrangements found by comparing the audio
# Used by the "tools" service in compose.yaml.
FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir numpy scipy librosa soundfile
COPY tools/mb-match/mb_match.py tools/audio-match/audio_match.py tools/audio-match/test_audio_match.py /opt/eos-tools/
RUN printf '#!/bin/sh\nexec python3 /opt/eos-tools/mb_match.py "$@"\n' > /usr/local/bin/mb-match \
    && printf '#!/bin/sh\nexec python3 /opt/eos-tools/audio_match.py "$@"\n' > /usr/local/bin/audio-match \
    && chmod +x /usr/local/bin/mb-match /usr/local/bin/audio-match
ENV NUMBA_CACHE_DIR=/tmp/numba EOS_SERVER=http://eos:4540 EOS_MUSIC=/music EOS_AUDIO_CACHE=/data/cache/audio-match
WORKDIR /opt/eos-tools
CMD ["sh", "-c", "echo 'usage: docker compose run --rm tools mb-match|audio-match --help'"]
