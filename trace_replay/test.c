#include <stdio.h>

#include <stdlib.h> // rand() 함수 포함 라이브러리



int main()

{

	int random = 0; // 정수형 변수 선언

	for (int i = 0; i < 10; i++) { // 10번 반복

		random = rand()%9; // 난수 생성

		printf("%d\n", random); // 출력

	}

}


