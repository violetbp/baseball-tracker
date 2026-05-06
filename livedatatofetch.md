
---currentplay---
liveData,plays,currentPlay,
result,description
about,isScoringPlay,isComplete
count,balls,strikes,outs
matchup,batter,id
,pitcher,id


playEvents,hitData,launchSpeed,totalDistance,trajectory

- if its a scoring play, we do the desc (iscomplete might matter?)
// playevents is only interesting when an event happens...




---status---
gameData,status,abstractGameState,detailedState


-------FETCH ONCE AT STARTUP-------
---playername---
gameData,players

*need to do lots of parsing to get the player names