/*
 * SVC.c
 *
 *  Created on: Oct 20, 2018
 *      Author: NickB
 */


#include <stdio.h>
#include "kernel.h"
#include "kernelcalls.h"
#include "ISRs.h"
#include "MSGpasser.h"

//extern void systick_init();


void SVCall(void)
{
    /* Supervisor call (trap) entry point
     * Using MSP - trapping process either MSP or PSP (specified in LR)
     * Source is specified in LR: F9 (MSP) or FD (PSP)
     * Save r4-r11 on trapping process stack (MSP or PSP)
     * Restore r4-r11 from trapping process stack to CPU
     * SVCHandler is called with r0 equal to MSP or PSP to access any arguments
     */
   // send_msg('x', MONsrc, UARTq); // indicator that we are in the SVC call (debuging puerposes)
    /* Save LR for return via MSP or PSP */
    __asm("     PUSH    {LR}");

    /* Trapping source: MSP or PSP? */
    __asm("     TST     LR,#4");    /* Bit #4 indicates MSP (0) or PSP (1) */
    __asm("     BNE     RtnViaPSP");

    /* Trapping source is MSP - save r4-r11 on stack (default, so just push) */
    __asm("     PUSH    {r4-r11}");
    __asm("     MRS r0,msp");
    __asm("     BL  SVCHandler");   /* r0 is MSP */
    __asm("     POP {r4-r11}");
    __asm("     POP     {PC}");

    /* Trapping source is PSP - save r4-r11 on psp stack (MSP is active stack) */
    __asm("RtnViaPSP:");
    __asm("     mrs     r0,psp");
    __asm("     stmdb   r0!,{r4-r11}"); /* Store multiple, decrement before */
    __asm("     msr psp,r0");
    __asm("     BL  SVCHandler");   /* r0 Is PSP */

    /* Restore r4..r11 from trapping process stack  */
    __asm("     mrs     r0,psp");
    __asm("     ldmia   r0!,{r4-r11}"); /* Load multiple, increment after */
    __asm("     msr psp,r0");
    __asm("     POP     {PC}");

}

void SVCHandler(struct stack_frame *argptr)
{
    /*
     * Supervisor call handler
     * Handle startup of initial process
     * Handle all other SVCs such as getid, terminate, etc.
     * Assumes first call is from startup code
     * Argptr points to (i.e., has the value of) either:
       - the top of the MSP stack (startup initial process)
       - the top of the PSP stack (all subsequent calls)
     * Argptr points to the full stack consisting of both hardware and software
       register pushes (i.e., R0..xPSR and R4..R10); this is defined in type
       stack_frame
     * Argptr is actually R0 -- setup in SVCall(), above.
     * Since this has been called as a trap (Cortex exception), the code is in
       Handler mode and uses the MSP
     */
    static int firstSVCcall = TRUE;

    struct pkargs *kcaptr; // points at our shared memory between p and k space

    struct pkSend *pkSptr; // points at the structure referred to in pkargs
    struct pkRecv *pkRptr; // points at the structure referred to in pkargs
    struct CUPch *pkUsend; // points at the structure referred to in pkargs

    if (firstSVCcall)
    {
    /*
     * Force a return using PSP
     * This will be the first process to run, so the eight "soft pulled" registers
       (R4..R11) must be ignored otherwise PSP will be pointing to the wrong
       location; the PSP should be pointing to the registers R0..xPSR, which will
       be "hard pulled"by the BX LR instruction.
     * To do this, it is necessary to ensure that the PSP points to (i.e., has) the
       address of R0; at this moment, it points to R4.
     * Since there are eight registers (R4..R11) to skip, the value of the sp
       should be increased by 8 * sizeof(unsigned int).
     * sp is increased because the stack runs from low to high memory.
    */
        set_PSP((unsigned long)(running[high_priority] -> sp) + 8 * sizeof(unsigned int));

        firstSVCcall = FALSE;
        /* Start SysTick */
        /* Initialize SYSTICK */
        SysTickPeriod(MAX_WAIT);
        SysTickIntEnable();
        SysTickStart();


        /*
         - Change the current LR to indicate return to Thread mode using the PSP
         - Assembler required to change LR to FFFF.FFFD (Thread/PSP)
         - BX LR loads PC from PSP stack (also, R0 through xPSR) - "hard pull"
        */
        __asm(" movw    LR,#0xFFFD");  /* Lower 16 [and clear top 16] */
        __asm(" movt    LR,#0xFFFF");  /* Upper 16 only */
        __asm(" bx  LR");          /* Force return to PSP */
    }
    else /* Subsequent SVCs */
    {
    /*
     * kcaptr points to the arguments associated with this kernel call
     * argptr is the value of the PSP (passed in R0 and pointing to the TOS)
     * the TOS is the complete stack_frame (R4-R10, R0-xPSR)
     * in this example, R7 contains the address of the structure supplied by
        the process - the structure is assumed to hold the arguments to the
        kernel function.
     * to get the address and store it in kcaptr, it is simply a matter of
       assigning the value of R7 (arptr -> r7) to kcaptr
     */

    //#ifdef FOR_KERNEL_ARGS
        kcaptr = (struct pkargs *) argptr -> r7;

        switch(kcaptr -> code)
        {
        case GETID:
            kcaptr -> rtnvalue = running[high_priority]->id;
        break;
        case BIND:
            kcaptr -> rtnvalue = k_bind(*(int*)kcaptr -> ptr_to_structure); // this calls the bind function in kernel and returns TRUE for sucessful bind and FALSE for failed bind
        break;
        case NICE:
            k_nice(*(int*)kcaptr -> ptr_to_structure);
        break;
        case TERMINATE:

            k_terminater(); // this function will deallocate the stk and PCB
        break;
        case SEND:
            pkSptr = kcaptr -> ptr_to_structure;
            kcaptr -> rtnvalue =  k_send(pkSptr->to, pkSptr->from, pkSptr->msg, pkSptr->size); // this function will facilitate the sending of messages;
        break;
        case RECV:
            pkRptr = kcaptr -> ptr_to_structure;
            kcaptr -> rtnvalue = k_recv(pkRptr->my_mailbox, pkRptr->from, pkRptr->msg, pkRptr->size); // this function will facilitate the receiving of messages
        break;
        case UART_OUT_CH:
            pkUsend = kcaptr -> ptr_to_structure;
            kcaptr -> rtnvalue = k_uart_organizer(pkUsend->ch, pkUsend->cmdchar, pkUsend->col[0], pkUsend->col[1], pkUsend->esc, pkUsend->line[0], pkUsend->line[1], pkUsend->semicolon, pkUsend->sqrbrkt, pkUsend->UARTnum);
        break;
        default:
            kcaptr -> rtnvalue = -1;
        }
    //#endif

    }

}
