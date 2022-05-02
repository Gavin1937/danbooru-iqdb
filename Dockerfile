FROM ubuntu:20.04 AS build
ARG CMAKE_VERSION=3.21.1

WORKDIR /iqdb
ENV TZ=America/Los_Angeles
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone && \
  apt-get update -y && \
  apt-get install -y tzdata
RUN \
  apt-get install --yes --no-install-recommends libssl-dev \
    wget ca-certificates build-essential cmake git python3 libgd-dev libsqlite3-dev binutils-dev && \
  wget https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz -O cmake.tar.gz && \
  tar -xzvf cmake.tar.gz -C /usr/local --strip-components=1
COPY . ./
RUN make release

FROM ubuntu:20.04
WORKDIR /iqdb
RUN \
  apt-get update && \
  apt-get install --yes --no-install-recommends libgd3 libsqlite3-0 sqlite3 binutils libssl-dev
COPY --from=build /iqdb/build/release/src/iqdb /usr/local/bin/

EXPOSE 5588
ENTRYPOINT ["iqdb"]
CMD ["http", "0.0.0.0", "5588", "iqdb.db"]
