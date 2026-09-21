# Bill of materials

Prices are rough single-unit figures in USD to give an idea of the total. Check
your local supplier.

| # | Part | Value / type | Qty | Approx. USD | Notes |
|---|---|---|---|---|---|
| 1 | Dish motor | DiSEqC 1.2 H-H mount motor | 1 | 50 | Hantech HD120 used on the prototype. Any DiSEqC 1.2 motor should work |
| 2 | MCU board | ESP32 dev board, WROOM-32 | 1 | 5 | Powered from its own USB port |
| 3 | L1, L2 | 330 uH toroidal inductor, 5 A | 2 | 2 | In series, supply + to coax centre. Must carry the motor current without saturating |
| 4 | C | 1 uF 250 V film capacitor | 1 | 1 | Blocks the motor DC from the GPIO pin. Film, not electrolytic |
| 5 | R | see README, 1/4 W | 1 | 0.1 | Between GPIO 25 and C |
| 6 | Supply | 12 to 18 V DC, 1 A or more | 1 | 8 | 15 to 18 V moves the dish better under load |
| 7 | Coax | RG6 with F connectors | as needed | 5 | Board to the motor's REC port |
| 8 | F connector | chassis or inline F female | 1 | 1 | Coax entry on the board |
| 9 | Screw terminals | 3-pin block | 3 | 1 | Layout in the README |

Total excluding the dish and mount: about 75 USD, most of it the motor.
