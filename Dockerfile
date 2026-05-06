FROM emscripten/emsdk:3.1.74

WORKDIR /src
COPY . .

RUN make site
