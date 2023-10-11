#rm ../simpleReplay
rm ../traceReplay
#g++ simpleReplay.cpp -g -o ../simpleReplay --static -lm
g++ -std=c++11 traceReplay.cpp cJSON.cpp traceConfig.cpp simpleReplay.cpp cacheReplay.cpp dbReplay.cpp -g -o ../traceReplay --static -lm
