# audio-embed: one vector per song from a pretrained audio model (CPU build of PyTorch).
#   docker build -t eos-embed tools/audio-embed
FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends libsndfile1 \
    && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir --index-url https://download.pytorch.org/whl/cpu torch==2.5.1 \
    && pip install --no-cache-dir transformers==4.46.3 numpy soundfile soxr
COPY audio_embed.py /opt/eos-tools/audio_embed.py
ENV HF_HOME=/cache/hf
ENTRYPOINT ["python3", "/opt/eos-tools/audio_embed.py"]
