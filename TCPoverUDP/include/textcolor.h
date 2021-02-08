/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   textcolor.h
 * Author: sk
 *
 * Created on November 11, 2018, 6:15 AM
 */

#ifndef TEXTCOLOR_H
#define TEXTCOLOR_H

#ifdef __cplusplus
extern "C" {
#endif
    
/// TEXT COLOR
#define RESET		0
#define BRIGHT 		1
#define DIM		2
#define UNDERLINE 	3
#define BLINK		4
#define REVERSE		7
#define HIDDEN		8

#define BLACK 		0
#define RED		1
#define GREEN		2
#define YELLOW		3
#define BLUE		4
#define MAGENTA		5
#define CYAN		6
#define	WHITE		7

void textcolor(int attr, int fg, int bg);


#ifdef __cplusplus
}
#endif

#endif /* TEXTCOLOR_H */

