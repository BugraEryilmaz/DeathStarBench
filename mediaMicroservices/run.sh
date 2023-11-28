#!/bin/bash

# set cpu frequency
sudo cpupower frequency-set --min 2.45G
sudo cpupower frequency-set --max 2.45G

# run docker containers
docker-compose down
docker volume rm $(docker volume ls -q)
docker-compose up -d

sleep 5
# fill the databases
python3 scripts/write_movie_info.py -c datasets/tmdb/casts.json -m datasets/tmdb/movies.json --server_address http://localhost:8080 && scripts/register_users.sh && scripts/register_movies.sh

# check cpu frequency
sudo cpupower -c 1 frequency-info
