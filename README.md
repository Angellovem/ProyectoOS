## Proyecto Sistemas Operativos

### Funcionamiento:
1. Limpiar codigo anterior y re-compilar el proyecto
```
make clean
make
```
2. Inciar el controlador. Los datos de controlador significan: -i: Hora Incial, -f: Hora Final, -s: Valor que indica a cuantos segundos equivale una hora en la simulacion, -t: Total de personas que caben como maximo en el parque, -p: Pipe del programa.
```
./controlador -i 7 -f 19 -s 1 -t 50 -p /tmp/pipe1
```
3. Inciar agentes con csvs de prueba (esto debe hacerse en una terminal diferente al controlador y cada agente debe tener su propia terminal). Los datos de los agentes significan: -s: Nombre del agente, -a: Archivo donde se encuentran las reservaciones (el formato de este es: Familia,hora,personas), -p: Pipe del programa.
```
./agente -s AgenteA -a solicitudesA.csv -p /tmp/pipe1
./agente -s AgenteB -a solicitudesB.csv -p /tmp/pipe1
```

Una vez se corre el programa y los agentes se deberia ver hora por hora las ocurrencias dentro del parque como la entrada de familias, la salida de estas, reprogramaciones, etc.
