PREFIX=${1}
SERVER="S4"
scp -r -P 1100 YJ/result yjkang@115.145.178.61:~/mtftl/${PREFIX}_S${SERVER}_result
scp -r -P 1100 trace_* yjkang@115.145.178.61:~/mtftl/trace/${PREFIX}_S${SERVER}_trace


