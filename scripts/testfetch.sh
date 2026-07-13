#!/usr/bin/env bash
FIELDS="metaData,timeStamp,wait,gamePk,gameData,status,abstractGameState,detailedState,liveData,plays,currentPlay,result,description,about,isScoringPlay,isComplete,count,balls,strikes,outs,matchup,batter,id,pitcher,id,playEvents,hitData,launchSpeed,totalDistance,trajectory"

curl -s "https://statsapi.mlb.com/api/v1.1/game/823719/feed/live?fields=${FIELDS}" | python3 -m json.tool


"metaData,timeStamp,wait,gamePk,
liveData,plays,currentPlay,result,description,about,isScoringPlay,isComplete,count,balls,strikes,outs,matchup,batter,id,pitcher,id,
playEvents,hitData,launchSpeed,totalDistance,trajectory,


players,id,useLastName"


# FIELDS="gameData,players,id,useLastName,status,abstractGameState,detailedState,"
