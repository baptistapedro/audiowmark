FROM fuzzers/afl:2.52

RUN apt-get update
RUN apt install -y build-essential wget git clang cmake  automake autotools-dev pkg-config  libtool zlib1g zlib1g-dev libexif-dev libjpeg-dev gettext
RUN  git clone  https://github.com/swesterfeld/audiowmark.git
WORKDIR /audiowmark
RUN ./autogen.sh
RUN ./configure CC=afl-clang CXX=afl-clang++
RUN make
RUN make install
RUN mkdir /audiowmarkCorpus
RUN wget https://www2.cs.uic.edu/~i101/SoundFiles/StarWars3.wav
RUN wget https://www2.cs.uic.edu/~i101/SoundFiles/preamble10.wav
RUN mv *.wav /audiowmarkCorpus


ENTRYPOINT  ["afl-fuzz", "-m", "2048", "-t", "3000+", "-i", "/audiowmarkCorpus", "-o" "/audiowmarkOut"]
CMD ["/audiowmark/src/audiowmark", "get", "@@"]
