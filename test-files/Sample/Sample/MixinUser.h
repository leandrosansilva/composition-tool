//
//  MixinUser.h
//  Sample
//
//

#import <Foundation/Foundation.h>

#import "MixinWithSomething.h"

#define PROVIDE(__value__) __attribute__((annotate("__provide__ " #__value__)))

@interface MixinUser : NSObject

@property (readonly) MixinWithSomething* mws
	PROVIDE(@length invertName concatenateWithPrefix:suffix:);

/* Generated in the file generated/MixinUser+mixin_property_mws_MixinWithSomething.h
 @property (readonly) NSNumber* length;
 - (NSString*)invertName;
 - (NSString*)concatenateWithPrefix:(NSString*)prefix suffix:(NSString*)suffix;
*/

@end

/**
 The property/ivar can be declarated in either header or in the implementation file.
 In case the "mixed" items must be public, a header file for the extension must be
 included by the client.
 
 TODO: how to handle properties which are of a protocoled type? '<' and '>' cannot be
 used in file names. Maybe replace with __?
*/
