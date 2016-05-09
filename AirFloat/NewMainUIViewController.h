//
//  NewMainUIViewController.h
//  AirFloat
//
//  Created by Jerry on 5/9/16.
//
//

#import <UIKit/UIKit.h>

#import <libairfloat/raopserver.h>

@interface NewMainUIViewController : UIViewController

@property (assign,nonatomic,readwrite) raop_server_p server;

@end
