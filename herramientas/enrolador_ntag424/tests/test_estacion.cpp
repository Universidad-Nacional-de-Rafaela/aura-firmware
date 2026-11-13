#include "../estacion_logica.h"
#include "../../../comun/tests/aserciones.h"

int main()
{
    est_boton_t b = {false, 0};
    VERIFICAR(est_modo(&b, 1000) == EST_ENROLAR);                 /* suelto: enrola */

    est_boton_actualizar(&b, true, 1000);                          /* se apreta */
    VERIFICAR(est_boton_ms(&b, 1000) == 0);
    VERIFICAR(est_modo(&b, 1399) == EST_ENROLAR);                  /* un roce no alcanza */
    VERIFICAR(est_modo(&b, 1400) == EST_BORRAR);                   /* sostenido: borra */
    est_boton_actualizar(&b, true, 1700);                          /* sigue apretado: no reinicia */
    VERIFICAR(est_boton_ms(&b, 2000) == 1000);
    est_boton_actualizar(&b, false, 2100);                         /* se suelta */
    VERIFICAR(est_modo(&b, 2100) == EST_ENROLAR);
    est_boton_actualizar(&b, true, 2200);                          /* se vuelve a apretar: cuenta de cero */
    VERIFICAR(est_modo(&b, 2300) == EST_ENROLAR);

    /* el reloj de 32 bits da la vuelta sin romper la cuenta */
    est_boton_t c = {false, 0};
    est_boton_actualizar(&c, true, 0xFFFFFF00u);
    VERIFICAR(est_modo(&c, 0x00000200u) == EST_BORRAR);

    /* sonidos */
    VERIFICAR(est_sonido(EST_ENROLAR, ENR_ENROLADA_OK) == EST_SON_ENROLADA);
    VERIFICAR(est_sonido(EST_ENROLAR, ENR_YA_ENROLADA) == EST_SON_YA);
    VERIFICAR(est_sonido(EST_BORRAR, ENR_DESENROLADA_OK) == EST_SON_BORRADA);
    VERIFICAR(est_sonido(EST_BORRAR, ENR_YA_DE_FABRICA) == EST_SON_YA);
    VERIFICAR(est_sonido(EST_ENROLAR, ENR_INCIERTA) == EST_SON_PELIGRO);
    VERIFICAR(est_sonido(EST_BORRAR, ENR_POSIBLEMENTE_PERDIDA) == EST_SON_PELIGRO);
    VERIFICAR(est_sonido(EST_ENROLAR, ENR_CLAVE_DESCONOCIDA) == EST_SON_ERROR);
    VERIFICAR(est_sonido(EST_ENROLAR, ENR_ERR_COMUNICACION) == EST_SON_ERROR);
    VERIFICAR(est_sonido(EST_BORRAR, ENR_CLAVE_DESCONOCIDA) == EST_SON_ERROR);
    VERIFICAR(est_sonido(EST_ENROLAR, ENR_ERR_SELECCION) == EST_SON_ERROR);
    /* un resultado que no corresponde al modo nunca suena a exito */
    VERIFICAR(est_sonido(EST_ENROLAR, ENR_DESENROLADA_OK) == EST_SON_ERROR);
    VERIFICAR(est_sonido(EST_BORRAR, ENR_ENROLADA_OK) == EST_SON_ERROR);

    /* retiro de la tarjeta: tres lecturas vacias seguidas */
    int a = 0;
    VERIFICAR(!est_retirada(&a, false));
    VERIFICAR(!est_retirada(&a, true));                            /* volvio: cuenta de cero */
    VERIFICAR(!est_retirada(&a, false));
    VERIFICAR(!est_retirada(&a, false));
    VERIFICAR(est_retirada(&a, false));

    RESUMEN();
}
