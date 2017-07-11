//
//  ClassWithSomething.h
//  Sample
//
//

#import <Foundation/Foundation.h>

@protocol SomeDelegate<NSObject>
- (void)doSomething;
- (void)doSomething:(NSNumber*)smth Else:(NSString*)Else;
@end

typedef struct SomeStruct
{
	struct SomeStruct* next;
	int value;
} SomeStruct;

@interface MixinWithSomething : NSObject

- (instancetype)initWithName:(NSString*)name;

- (NSString*)invertName;

- (NSString*)concatenateWithPrefix:(NSString*)prefix
                             suffix:(NSString*)suffix;

- (SomeStruct)buildSomeStruct;

@property (readonly) NSNumber* length;

@end

@interface MixinWithSomething (SomeWeirdExtension)

+ (BOOL) staticMemberForRescue:(MixinWithSomething*)parent;

@end
