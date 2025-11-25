# Lista de que falta por hacer (Porfa actualizar cuando este listo)

Parsear sus propios argumentos (-s nombre -a fileSolicitud -p pipeRecibe)
Crear su FIFO de respuesta
Enviar REG|nombre|ruta_fifo al pipeRecibe y leer TIME|horaActual
Leer su archivo CSV, construir cada REQ|... respetando que la hora no sea anterior a horaActual, enviarlo y esperar la línea de respuesta que puede dar diferentes tipos de respuesta
EJP: 
   Si formato/rangos/aforo inválidos:
   "RESP|NEG|<familia>|0|0"
   Si se acepta exactamente en la hora pedida:
   "RESP|OK|<familia>|<horaInicio>|<horaFin>"
   Si se reprograma a otro bloque de 2 horas:
   "RESP|REPROG|<familia>|<horaInicio>|<horaFin>"
   Si es extemporánea y ya no hay bloque alternativo:
   "RESP|NEG_EXTEMP|<familia>|0|0"
Imprimir la respuesta bonita.
Detenerse cuando reciba END|FIN_SIMULACION y mostrar Agente <nombre> termina.
