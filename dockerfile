FROM ubuntu:24.04
# Above ubuntu:23.03+ images have switched to using the new sources.list.d format.
# before ubuntu:23.03, we would modify /etc/apt/sources.list directly.
# Here we switch to Tsinghua University mirrors for faster access in China.
# See https://launchpad.net/ubuntu/+archivemirrors to find other mirrors.
RUN sed -i 's|archive.ubuntu.com|mirrors.tuna.tsinghua.edu.cn|g; s|security.ubuntu.com|mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources && \
    apt update && \
    apt install -y build-essential vim git curl wget \
    universal-ctags cscope cflow doxygen graphviz ripgrep
WORKDIR /workspace
CMD ["/bin/bash"]