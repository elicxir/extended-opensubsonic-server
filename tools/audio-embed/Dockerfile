# audio-embed: one vector per song from a pretrained audio model.
#   docker build -t eos-embed tools/audio-embed                              # CUDA build (default)
#   docker build -t eos-embed --build-arg TORCH_INDEX=cpu tools/audio-embed  # CPU only, smaller
# With CUDA, run the container with --gpus all; without a GPU it falls back to the CPU.
FROM python:3.12-slim
ARG TORCH_INDEX=cu124
RUN apt-get update && apt-get install -y --no-install-recommends libsndfile1 \
    && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir --index-url https://download.pytorch.org/whl/${TORCH_INDEX} torch==2.5.1 \
    && pip install --no-cache-dir transformers==4.46.3 numpy soundfile soxr
COPY audio_embed.py /opt/eos-tools/audio_embed.py
ENV HF_HOME=/cache/hf
ENTRYPOINT ["python3", "/opt/eos-tools/audio_embed.py"]
